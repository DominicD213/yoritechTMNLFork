#include <Arduino.h>
#if defined(AVOLYN_HEADLESS_WIFI_TEST) && AVOLYN_HEADLESS_WIFI_TEST
// Avolyn's temporary display-free USB -> Wi-Fi provisioning firmware.
// Does not run stock TRMNL display, QA, setup portal, or image polling.
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <esp_http_server.h>
#include <esp_system.h>

#include "avolyn_frame_usb.h"
#include "avolyn_pairing_crypto.h"
#include "avolyn_wifi_credentials.h"
#include "avolyn_wifi_reconnect.h"

namespace {
  constexpr char FIRMWARE_VERSION[] = "0.2.5-dev";
  avolyn::FrameReceiver frames(avolyn::frameSha256);
  constexpr unsigned long WIFI_POLL_MS = 250; // 4 inexpensive status checks/sec instead of ~500/sec.
  constexpr unsigned long LAN_RETRY_MS = 5000;
  constexpr size_t SERIAL_LINE_MAX = 1024;

  Preferences wifiPrefs;
  httpd_handle_t lanServer = nullptr;
  Preferences pairingPrefs;
  SemaphoreHandle_t pairingMutex = nullptr;
  String pairedKeyId;
  String pairedSecret;
  avolyn::Challenge challenge;
  uint32_t lastAuthAttempt = 0;
  bool authAttempted = false;
  struct PairingGuard {
    bool locked;
    PairingGuard() : locked(pairingMutex && xSemaphoreTake(pairingMutex, pdMS_TO_TICKS(30)) == pdTRUE) {}
    ~PairingGuard() {
      if (locked) xSemaphoreGive(pairingMutex);
    }
  };
  char serialLine[SERIAL_LINE_MAX + 1] = {};
  size_t serialLength = 0;
  bool discardOversizedLine = false;

  bool wifiConnecting = false;
  bool saveAfterConnection = false;
  bool savedWifiConfigured = false;
  bool wifiWasConnected = false;
  bool mdnsStarted = false;
  bool wifiStorageError = false;
  std::atomic<uint8_t> wifiEvents{0};
  std::atomic<uint16_t> lastDisconnectReason{0};
  std::atomic<uint32_t> associatedAt{0};
  std::atomic<bool> wifiAssociated{false};
  unsigned long connectionStart = 0;
  unsigned long lastWifiFailureAt = 0;
  unsigned long lastWifiPoll = 0;
  unsigned long lastLanAttempt = 0;
  uint8_t wifiFailures = 0; // Saturating count; never blocks recovery.
  unsigned long lastJoinDurationMs = 0;
  unsigned long lastFailedAttemptMs = 0;
  int lastFailedWiFiStatus = -1;
  bool hasJoinDuration = false;
  String savedSsid;
  String savedPassword;
  String pendingSsid;
  String pendingPassword;

  void sendJson(JsonDocument &doc) {
    if (!Serial) return; // Wall-power recovery must never wait for a USB host.
    serializeJson(doc, Serial);
    Serial.println();
  }

  void sendError(const char *reason) {
    JsonDocument result;
    result["type"] = "ERROR";
    result["message"] = reason;
    sendJson(result);
  }

  const char *connectionState() {
    if (WiFi.status() == WL_CONNECTED) return "connected";
    if (wifiConnecting) return "connecting";
    return "disconnected";
  }

  void sendWifiStatus() {
    JsonDocument result;
    result["type"] = "WIFI_STATUS";
    result["state"] = connectionState();
    result["connected"] = (WiFi.status() == WL_CONNECTED);
    result["ssid"] = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "";
    result["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
    result["macAddress"] = WiFi.macAddress();
    result["reconnectFailures"] = wifiFailures;
    result["retryDelayMs"] = avolyn::wifiRetryDelayMs(wifiFailures);
    result["automaticRetryEnabled"] = savedWifiConfigured;
    result["credentialsSaved"] =
      !wifiStorageError && savedWifiConfigured && (WiFi.status() != WL_CONNECTED || WiFi.SSID() == savedSsid);
    result["storageError"] = wifiStorageError;
    result["lastDisconnectReason"] = lastDisconnectReason.load();
    result["resetReason"] = static_cast<int>(esp_reset_reason());
    result["uptimeMs"] = millis();
    result["connectionElapsedMs"] = wifiConnecting ? uint32_t(millis() - connectionStart) : 0;
    result["lastJoinDurationMs"] = hasJoinDuration ? lastJoinDurationMs : 0;
    result["hasJoinDuration"] = hasJoinDuration;
    result["lastFailedAttemptMs"] = lastFailedAttemptMs;
    result["lastFailedWiFiStatus"] = lastFailedWiFiStatus;
    result["signalDbm"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    sendJson(result);
  }

// Responds to the exact legacy commands used by Electron v0.7.0.
  void sendFirmwareStatus() {
    JsonDocument result;
    result["device"] = "Avolyn Slate";
    result["board"] = "xiao-esp32s3";
    result["display"] = "headless-wifi-test";
    result["pairingProtocol"] = avolyn::PROTOCOL;
    result["firmwareVersion"] = FIRMWARE_VERSION;
    result["status"] = connectionState();
    result["displayConnected"] = false;
    result["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    result["macAddress"] = WiFi.macAddress();
    sendJson(result);
  }

// USB is the trust bootstrap. There is intentionally no LAN key provisioning route.
  void pairUsb(const JsonDocument &command, bool forget) {
    PairingGuard guard;
    if (!guard.locked) {
      sendError("Pairing is busy. Retry.");
      return;
    }
    bool ok = false;
    if (forget) {
      ok = !pairingPrefs.isKey("record") || pairingPrefs.remove("record");
      if (ok) {
        pairedKeyId = "";
        pairedSecret = "";
      }
    } else {
      const char *keyId = command["keyId"] | "";
      const char *secret = command["secret"] | "";
      if (!avolyn::isHex(keyId, 16) || !avolyn::isHex(secret, 32)) {
        sendError("Invalid pairing credentials.");
        return;
      }
        // One atomic NVS value prevents mismatched key IDs and secrets on power loss.
      String record = String(keyId) + secret;
      ok = pairingPrefs.putString("record", record) == record.length();
      if (ok) {
        pairedKeyId = keyId;
        pairedSecret = secret;
      }
    }
    if (ok) {
      challenge.active = false;
      authAttempted = false;
    }
    JsonDocument result;
    result["type"] = forget ? "PAIR_FORGET" : "PAIR_SET";
    result["ok"] = ok;
    result["keyId"] = pairedKeyId;
    result["macAddress"] = WiFi.macAddress();
    sendJson(result); // Never return the pairing secret.
  }

  esp_err_t sendLanJson(httpd_req_t *req, const String &body, const char *status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body.c_str(), body.length());
  }

  esp_err_t handleChallenge(httpd_req_t *req) {
    String body;
    {
      PairingGuard guard;
      if (!guard.locked || pairedSecret.isEmpty()) return sendLanJson(req, "{}", "401 Unauthorized");
      if (!challenge.valid(millis())) {
        uint8_t random[32];
        esp_fill_random(random, sizeof(random)); // Wi-Fi is active, enabling the ESP32 hardware entropy source.
        challenge.issue(random, millis());
      }
      JsonDocument response;
      response["protocol"] = avolyn::PROTOCOL;
      response["challenge"] = challenge.value;
      serializeJson(response, body);
    }
    return sendLanJson(req, body);
  }

  esp_err_t handleLanStatus(httpd_req_t *req) {
    // ESP-IDF's HTTP parser handles framing. Bound content before reading/allocating it.
    if (req->content_len == 0 || req->content_len > 3072) return sendLanJson(req, "{}", "413 Payload Too Large");
    char contentType[40] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", contentType, sizeof(contentType)) != ESP_OK ||
        strcmp(contentType, "application/json") != 0)
      return sendLanJson(req, "{}", "415 Unsupported Media Type");
    char wire[3073];
    size_t received = 0;
    const uint32_t receiveStarted = millis();
    while (received < req->content_len) {
      if (uint32_t(millis() - receiveStarted) > 3000) return ESP_FAIL;
      const int count = httpd_req_recv(req, wire + received, req->content_len - received);
      if (count <= 0) return ESP_FAIL; // Close on timeout or incomplete body.
      received += count;
    }
    wire[received] = 0;
    JsonDocument envelope;
    if (deserializeJson(envelope, wire, received)) return sendLanJson(req, "{}", "400 Bad Request");
    String responseBody;
    {
      PairingGuard guard;
      const uint32_t now = millis();
      if (!guard.locked || pairedSecret.isEmpty() || !challenge.valid(now))
        return sendLanJson(req, "{}", "401 Unauthorized");
      if (authAttempted && uint32_t(now - lastAuthAttempt) < 100)
        return sendLanJson(req, "{}", "429 Too Many Requests");
      authAttempted = true;
      lastAuthAttempt = now;
      if (!envelope["v"].is<int>() || envelope["v"].as<int>() != 1 || pairedKeyId != (envelope["keyId"] | ""))
        return sendLanJson(req, "{}", "401 Unauthorized");
      const char *requestIv = envelope["iv"] | "";
      uint8_t key[32], plaintext[avolyn::MAX_PLAINTEXT + 1];
      const String aad = String(avolyn::PROTOCOL) + "\nrequest\n" + pairedKeyId + "\n" + challenge.value;
      size_t length = 0;
      if (!avolyn::deriveKey(pairedSecret.c_str(), pairedKeyId.c_str(), false, key)) return ESP_FAIL;
      const bool authenticated = avolyn::decrypt(key, requestIv, envelope["tag"] | "", envelope["ciphertext"] | "",
                                                 aad.c_str(), plaintext, length);
      avolyn::wipe(key, sizeof(key));
      if (!authenticated) return sendLanJson(req, "{}", "401 Unauthorized");
      JsonDocument command;
      const auto error = deserializeJson(command, reinterpret_cast<const char *>(plaintext), length);
      avolyn::wipe(plaintext, sizeof(plaintext));
      if (error || strcmp(command["op"] | "", "status") != 0 || !challenge.consume(command["challenge"] | "", millis()))
        return sendLanJson(req, "{}", "401 Unauthorized");
      JsonDocument status;
      status["product"] = "avolyn-slate";
      status["board"] = "xiao-esp32s3";
      status["firmwareVersion"] = FIRMWARE_VERSION;
      status["macAddress"] = WiFi.macAddress();
      status["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
      status["wifiJoinTimeKnown"] = hasJoinDuration;
      status["wifiJoinDurationMs"] = hasJoinDuration ? lastJoinDurationMs : 0;
      status["wifiSignalDbm"] = WiFi.RSSI();
      status["credentialsSaved"] = !wifiStorageError && savedWifiConfigured;
      status["storageError"] = wifiStorageError;
      status["wifiDisconnectReason"] = lastDisconnectReason.load();
      status["resetReason"] = static_cast<int>(esp_reset_reason());
      status["uptimeMs"] = millis();
      status["paired"] = true;
      String plainStatus;
      serializeJson(status, plainStatus);
      const String responseAad =
        String(avolyn::PROTOCOL) + "\nresponse\n" + pairedKeyId + "\n" + challenge.value + "\n" + requestIv;
      uint8_t iv[12];
      esp_fill_random(iv, sizeof(iv));
      char cipherHex[avolyn::MAX_PLAINTEXT * 2 + 1], tagHex[33], ivHex[25];
      if (!avolyn::deriveKey(pairedSecret.c_str(), pairedKeyId.c_str(), true, key)) return ESP_FAIL;
      const bool encrypted =
        avolyn::encrypt(key, iv, responseAad.c_str(), reinterpret_cast<const uint8_t *>(plainStatus.c_str()),
                        plainStatus.length(), cipherHex, tagHex);
      avolyn::wipe(key, sizeof(key));
      if (!encrypted) return ESP_FAIL;
      avolyn::toHex(iv, sizeof(iv), ivHex);
      JsonDocument response;
      response["v"] = 1;
      response["keyId"] = pairedKeyId;
      response["iv"] = ivHex;
      response["ciphertext"] = cipherHex;
      response["tag"] = tagHex;
      serializeJson(response, responseBody);
    }
    return sendLanJson(req, responseBody);
  }

  void startLanServer() {
    if (lanServer || WiFi.status() != WL_CONNECTED || !pairingMutex) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.stack_size = 16384;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    if (httpd_start(&lanServer, &config) != ESP_OK) {
      lanServer = nullptr;
      return;
    }
    httpd_uri_t challengeRoute = {};
    challengeRoute.uri = "/challenge";
    challengeRoute.method = HTTP_GET;
    challengeRoute.handler = handleChallenge;
    const esp_err_t challengeRegistered = httpd_register_uri_handler(lanServer, &challengeRoute);
    httpd_uri_t statusRoute = {};
    statusRoute.uri = "/status";
    statusRoute.method = HTTP_POST;
    statusRoute.handler = handleLanStatus;
    if (challengeRegistered != ESP_OK || httpd_register_uri_handler(lanServer, &statusRoute) != ESP_OK) {
      httpd_stop(lanServer);
      lanServer = nullptr;
      return;
    }
  }

  void startDiscovery() {
    if (mdnsStarted || !lanServer) return;
    String hostname = "avolyn-slate-" + WiFi.macAddress();
    hostname.replace(":", "");
    hostname.toLowerCase();
    mdnsStarted = MDNS.begin(hostname.c_str());
    if (mdnsStarted) {
      MDNS.addService("avolyn-slate", "tcp", 8080);
      MDNS.addServiceTxt("avolyn-slate", "tcp", "protocol", avolyn::PROTOCOL);
    }
  }

  void scanWifi() {
    const int count = WiFi.scanNetworks();
    if (count < 0) {
      sendError("Wi-Fi scan failed. Retry shortly.");
      return;
    }
    JsonDocument result;
    result["type"] = "WIFI_SCAN";
    JsonArray networks = result["networks"].to<JsonArray>();
    const int limit = count < 10 ? count : 10; // Small enough for existing Electron line capture.
    for (int i = 0; i < limit; ++i) {
      JsonObject network = networks.add<JsonObject>();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
      network["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    sendJson(result);
    WiFi.scanDelete();
  }

  void connectWifi(const String &ssid, const String &password, bool rememberAfterSuccess, bool announce = true) {
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
        (!password.isEmpty() && password.length() < 8)) {
      if (announce) sendError("Invalid Wi-Fi name or password length.");
      return;
    }
    // Explicit USB setup and automatic reconnection share the same non-blocking
    // association path. Wi-Fi credentials are saved only after a verified join.
    pendingSsid = ssid;
    pendingPassword = password;
    saveAfterConnection = rememberAfterSuccess;
    wifiConnecting = true;
    connectionStart = millis();
    wifiAssociated.store(false);
    wifiEvents.store(0);
    WiFi.disconnect(false, false);
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());

    if (announce) {
      JsonDocument result;
      result["type"] = "WIFI_SET";
      result["accepted"] = true;
      result["state"] = "connecting";
      sendJson(result);
    }
  }

  void handleLine(const char *line) {
    if (strcmp(line, "PING") == 0) {
      Serial.println("PONG");
      return;
    }
    if (strcmp(line, "GET_STATUS") == 0) {
      sendFirmwareStatus();
      return;
    }
    if (strcmp(line, "WIFI_SCAN") == 0) {
      scanWifi();
      return;
    }
    if (strcmp(line, "WIFI_STATUS") == 0) {
      sendWifiStatus();
      return;
    }

    // WIFI_SET contains credentials and must be sent by the trusted Electron main
    // process over the physically connected USB port. Never echo its raw contents.
    JsonDocument command;
    const DeserializationError error = deserializeJson(command, line);
    if (error) {
      sendError("Unrecognized USB command.");
      return;
    }
    JsonDocument frameReply;
    if (avolyn::handleFrameUsb(command, frames, millis(), frameReply)) {
      sendJson(frameReply);
      return;
    }
    const char *name = command["cmd"] | "";
    if (strcmp(name, "PAIR_SET") == 0 || strcmp(name, "PAIR_FORGET") == 0) {
      pairUsb(command, strcmp(name, "PAIR_FORGET") == 0);
    } else if (strcmp(name, "WIFI_SET") == 0) {
      if (!command["ssid"].is<const char *>() || !command["password"].is<const char *>()) {
        sendError("WIFI_SET requires string ssid and password fields.");
        return;
      }
      connectWifi(command["ssid"].as<String>(), command["password"].as<String>(), true);
    } else if (strcmp(name, "WIFI_FORGET") == 0) {
      // Do not claim successful removal if durable credentials remain.
      for (const char *key : {"record", "ssid", "pass"}) {
        if (wifiPrefs.isKey(key) && !wifiPrefs.remove(key)) {
          wifiStorageError = true;
          sendError("Saved Wi-Fi settings could not be removed. Retry.");
          return;
        }
      }
      wifiStorageError = false;
      savedWifiConfigured = false;
      wifiFailures = 0;
      savedSsid = "";
      savedPassword = "";
      saveAfterConnection = false;
      pendingPassword = "";
      pendingSsid = "";
      wifiConnecting = false;
      WiFi.disconnect(false, true);
      JsonDocument result;
      result["type"] = "WIFI_FORGET";
      result["ok"] = true;
      sendJson(result);
    } else {
      sendError("Unsupported USB command.");
    }
  }

  void processUsb() {
    if (!Serial) return;
    while (Serial.available() > 0) {
      const char ch = static_cast<char>(Serial.read());
      if (ch == '\r') continue;
      if (ch == '\n') {
        if (!discardOversizedLine && serialLength > 0) {
          serialLine[serialLength] = '\0';
          handleLine(serialLine);
        }
        avolyn::wipe(serialLine, sizeof(serialLine));
        serialLength = 0;
        discardOversizedLine = false;
        continue;
      }
      if (discardOversizedLine) continue;
      if (serialLength < SERIAL_LINE_MAX) {
        serialLine[serialLength++] = ch;
      } else {
            // Never execute a truncated credentials command.
        avolyn::wipe(serialLine, sizeof(serialLine));
        serialLength = 0;
        discardOversizedLine = true;
        sendError("USB command exceeded maximum length.");
      }
    }
  }

  void updateWifi(unsigned long now) {
    const wl_status_t status = WiFi.status();
    const uint8_t events = wifiEvents.exchange(0);
    if ((events & 1) || (wifiWasConnected && status != WL_CONNECTED)) {
      if (wifiWasConnected) {
        wifiWasConnected = false;
        if (mdnsStarted) {
          MDNS.end();
          mdnsStarted = false;
        }
        if (lanServer) {
          httpd_stop(lanServer);
          lanServer = nullptr;
        }
        {
          PairingGuard guard;
          if (guard.locked) challenge.active = false;
        }
        lastWifiFailureAt = now;
        wifiFailures = 0;
        // A single transition notification; status polling is never required for recovery.
        if (status != WL_CONNECTED) sendWifiStatus();
      }
    }

    if (status == WL_CONNECTED) {
      if (!wifiWasConnected) {
        wifiWasConnected = true;
        lastLanAttempt = now - LAN_RETRY_MS; // Start promptly on association.
      }
      if ((!lanServer || !mdnsStarted) && uint32_t(now - lastLanAttempt) >= LAN_RETRY_MS) {
        lastLanAttempt = now;
        startLanServer(); // Retry briefly if the HTTP socket could not be created.
        startDiscovery();
      }
      if (wifiConnecting) {
        wifiConnecting = false;
        lastJoinDurationMs = uint32_t(now - connectionStart);
        hasJoinDuration = true;
        wifiFailures = 0;
        if (saveAfterConnection && (wifiStorageError || !savedWifiConfigured || savedSsid != pendingSsid ||
                                    savedPassword != pendingPassword)) {
          // Cache credentials; write NVS only for a newly successful network.
          const avolyn::WifiCredentials credentials{pendingSsid.c_str(), pendingPassword.c_str()};
          const bool persisted = avolyn::saveWifiCredentials(wifiPrefs, credentials);
          wifiStorageError = !persisted;
          if (persisted) {
            savedSsid = pendingSsid;
            savedPassword = pendingPassword;
            savedWifiConfigured = true;
          } else
            sendError("Wi-Fi connected, but settings could not be saved for wall-power reboot. Retry setup.");
        }
        saveAfterConnection = false;
        pendingSsid = "";
        pendingPassword = "";
        sendWifiStatus();
      }
      return;
    }

    if (wifiConnecting) {
      const bool rejected = (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL);
      const bool withinTimeout = !avolyn::wifiAttemptExpired(uint32_t(now - connectionStart), wifiAssociated.load(),
                                                             uint32_t(now - associatedAt.load()));
      const bool failedEarly = rejected && uint32_t(now - connectionStart) >= 2000;
      if (withinTimeout && !failedEarly) return;
      const bool userRequest = saveAfterConnection;
      lastFailedAttemptMs = uint32_t(now - connectionStart);
      lastFailedWiFiStatus = static_cast<int>(status);
      wifiConnecting = false;
      saveAfterConnection = false;
      pendingSsid = "";
      pendingPassword = "";
      lastWifiFailureAt = now;
      if (wifiFailures < 255) ++wifiFailures;
      WiFi.disconnect(false, false);
      if (userRequest) sendError("Wi-Fi connection failed. Check 2.4 GHz network/password.");
      return;
    }

    const uint32_t timeSinceFailure = now - lastWifiFailureAt;
    const uint32_t retryDelay = avolyn::wifiRetryDelayMs(wifiFailures);
    if (avolyn::shouldRetrySavedWifi(false, false, savedWifiConfigured, timeSinceFailure, retryDelay)) {
      // Credentials are cached after boot; no flash reads, active scans or USB
      // interactions on every retry. This continues at a low rate indefinitely.
      connectWifi(savedSsid, savedPassword, false, false);
    }
  }
} // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(10);
  // NVS below is our credential store; avoid an independent SDK flash/config path.
  WiFi.persistent(false);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    // Event callbacks run on another task. Keep them bounded; the main loop
    // owns NVS, HTTP teardown/startup and backoff, including without USB.
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      lastDisconnectReason.store(info.wifi_sta_disconnected.reason);
      wifiAssociated.store(false);
      wifiEvents.fetch_or(1);
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
      associatedAt.store(millis());
      wifiAssociated.store(true);
    }
  });
  WiFi.mode(WIFI_STA);
  // Our bounded-rate retry scheduler owns associations, not Arduino's rapid reconnect loop.
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  wifiStorageError = !wifiPrefs.begin("avolyn_wifi", false);

  pairingMutex = xSemaphoreCreateMutex();
  if (pairingMutex && pairingPrefs.begin("avolyn_pair", false)) {
    const String record = pairingPrefs.getString("record", "");
    if (record.length() == 96 && avolyn::isHex(record.substring(0, 32).c_str(), 16) &&
        avolyn::isHex(record.substring(32).c_str(), 32)) {
      pairedKeyId = record.substring(0, 32);
      pairedSecret = record.substring(32);
    }
  }
  avolyn::WifiCredentials credentials;
  const auto loaded = avolyn::loadWifiCredentials(wifiPrefs, credentials);
  savedWifiConfigured = loaded == avolyn::WifiLoad::Saved || loaded == avolyn::WifiLoad::Legacy;
  if (savedWifiConfigured) {
    savedSsid = credentials.ssid.c_str();
    savedPassword = credentials.password.c_str();
    if (loaded == avolyn::WifiLoad::Legacy && !avolyn::saveWifiCredentials(wifiPrefs, credentials))
      wifiStorageError = true;
  } else if (loaded == avolyn::WifiLoad::Invalid)
    wifiStorageError = true;
  if (savedWifiConfigured) {
    // Immediate attempt at boot; retries are event-paced, even with only wall power.
    connectWifi(savedSsid, savedPassword, false, false);
  }
  if (Serial) Serial.println("AVOLYN_SLATE_HEADLESS_READY");
}

void loop() {
  processUsb();
  const unsigned long now = millis();
  frames.expire(now);
  if (uint32_t(now - lastWifiPoll) >= WIFI_POLL_MS) {
    lastWifiPoll = now;
    updateWifi(now);
  }
  delay(10); // Yield to the ESP32 Wi-Fi/HTTP tasks; avoid CPU-intensive busy-polling.
}

#endif
