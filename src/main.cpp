#include <Arduino.h>

#include "bl.h"
#include "esp_ota_ops.h"
#include "power.h"
#include "qa.h"

#if defined(AVOLYN_HEADLESS_WIFI_TEST) && AVOLYN_HEADLESS_WIFI_TEST

// Avolyn's temporary display-free USB -> Wi-Fi provisioning firmware.
// Does not run stock TRMNL display, QA, setup portal, or image polling.
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebServer.h>

namespace {
constexpr char FIRMWARE_VERSION[] = "0.1.0-dev";
constexpr unsigned long WIFI_TIMEOUT_MS = 20000;
constexpr size_t SERIAL_LINE_MAX = 1024;

Preferences wifiPrefs;
WebServer lanServer(8080);
bool lanServerStarted = false;
char serialLine[SERIAL_LINE_MAX + 1] = {};
size_t serialLength = 0;
bool discardOversizedLine = false;

bool wifiConnecting = false;
bool saveAfterConnection = false;
unsigned long connectionStart = 0;
String pendingSsid;
String pendingPassword;

void sendJson(JsonDocument &doc) {
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
    sendJson(result);
}

// Responds to the exact legacy commands used by Electron v0.7.0.
void sendFirmwareStatus() {
    JsonDocument result;
    result["device"] = "Avolyn Slate";
    result["board"] = "xiao-esp32s3";
    result["display"] = "waveshare-579";
    result["firmwareVersion"] = FIRMWARE_VERSION;
    result["status"] = connectionState();
    result["displayConnected"] = false;
    result["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    result["macAddress"] = WiFi.macAddress();
    sendJson(result);
}

// Read-only LAN availability probe. Not a pairing or image-transfer endpoint.
void handleLanStatus() {
    JsonDocument response;
    response["product"] = "avolyn-slate";
    response["board"] = "xiao-esp32s3";
    response["firmwareVersion"] = FIRMWARE_VERSION;
    response["macAddress"] = WiFi.macAddress();
    response["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    response["paired"] = false;
    String body;
    serializeJson(response, body);
    lanServer.sendHeader("Cache-Control", "no-store");
    lanServer.send(200, "application/json", body);
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

void connectWifi(const String &ssid, const String &password, bool rememberAfterSuccess) {
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
        (!password.isEmpty() && password.length() < 8)) {
        sendError("Invalid Wi-Fi name or password length.");
        return;
    }
    pendingSsid = ssid;
    pendingPassword = password;
    saveAfterConnection = rememberAfterSuccess;
    wifiConnecting = true;
    connectionStart = millis();
    WiFi.disconnect(false, false);
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());

    JsonDocument result;
    result["type"] = "WIFI_SET";
    result["accepted"] = true;
    result["state"] = "connecting";
    sendJson(result);
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
    const char *name = command["cmd"] | "";
    if (strcmp(name, "WIFI_SET") == 0) {
        if (!command["ssid"].is<const char *>() || !command["password"].is<const char *>()) {
            sendError("WIFI_SET requires string ssid and password fields.");
            return;
        }
        connectWifi(command["ssid"].as<String>(), command["password"].as<String>(), true);
    } else if (strcmp(name, "WIFI_FORGET") == 0) {
        wifiPrefs.remove("ssid");
        wifiPrefs.remove("pass");
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
    while (Serial.available() > 0) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!discardOversizedLine && serialLength > 0) {
                serialLine[serialLength] = '\0';
                handleLine(serialLine);
            }
            serialLength = 0;
            discardOversizedLine = false;
            continue;
        }
        if (discardOversizedLine) continue;
        if (serialLength < SERIAL_LINE_MAX) {
            serialLine[serialLength++] = ch;
        } else {
            // Never execute a truncated credentials command.
            serialLength = 0;
            discardOversizedLine = true;
            sendError("USB command exceeded maximum length.");
        }
    }
}

void updateWifi() {
    if (!wifiConnecting) return;
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnecting = false;
        if (!lanServerStarted) { lanServer.begin(); lanServerStarted = true; }
        if (saveAfterConnection) {
            // Store only after successful association. Arduino Preferences uses NVS;
            // production builds should also enable appropriate flash encryption.
            wifiPrefs.putString("ssid", pendingSsid);
            wifiPrefs.putString("pass", pendingPassword);
        }
        saveAfterConnection = false;
        pendingPassword = "";
        pendingSsid = "";
        sendWifiStatus();
    } else if (millis() - connectionStart >= WIFI_TIMEOUT_MS) {
        wifiConnecting = false;
        saveAfterConnection = false;
        pendingPassword = "";
        pendingSsid = "";
        sendError("Wi-Fi connection timed out. Check network/password.");
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    wifiPrefs.begin("avolyn_wifi", false);

    lanServer.on("/status", HTTP_GET, handleLanStatus);
    Serial.println("AVOLYN_SLATE_HEADLESS_READY");
    const String savedSsid = wifiPrefs.getString("ssid", "");
    if (!savedSsid.isEmpty()) {
        connectWifi(savedSsid, wifiPrefs.getString("pass", ""), false);
    }
}

void loop() {
    processUsb();
    updateWifi();
    if (WiFi.status() == WL_CONNECTED) {
        if (!lanServerStarted) { lanServer.begin(); lanServerStarted = true; }
        lanServer.handleClient();
    }
    delay(2);
}

#else // Unmodified TRMNL startup for all non-headless builds.

#ifdef BOARD_TRMNL_X
#include "display.h"
#include "esp_sleep.h"
#include "filesystem.h"
#include "logo_medium.h"
#include "modem.h"

// TRMNL X setup (quite different from OG)
void setup() {
#ifndef NO_QA

  bool isShipped = checkIfAlreadyShipped();
  if (!isShipped) {
    bool shipmentStarted = checkIfShipmentStarted();

    display_init();

    if (shipmentStarted && power().usbStatus() == UsbStatus::CONNECTED) {
      // USB connected (e.g. battery died in transit and USB revived the device)
      enter_shipment_sleep();
      saveShipmentDone();
      ESP.restart();
    } else {
      filesystem_init();

      Serial.begin(115200);
      bool modemFlashed = checkIfModemFlashed();

      if (!modemFlashed) {
        Serial.println("[MODEM] Resetting modem...");
        modem_reset_target();
        delay(500);  // let modem reach bootloader

        display_show_msg(const_cast<uint8_t *>(logo_medium), MODEM_FLASHING);

        Modem modem(115200);
        String flashError;
        if (modem.flashFromFile("/system/factory_ESP32C5-4MB.bin", flashError)) {
          Serial.println("[MODEM] Factory flash complete.");
          saveModemFlashed();
        } else {
          Serial.println("[MODEM] Factory flash FAILED.");
          display_show_msg(const_cast<uint8_t *>(logo_medium), MODEM_FLASH_FAILED, flashError.c_str());
          delay(5000);
        }
      } else {
        Serial.println("[MODEM] Already flashed, skipping...");
      }
      // enableShipmentMode() will block until charger is connected
      enableShipmentMode();

      // Charger detected, save shipment status to preferences
      saveShipmentDone();
      ESP.restart();
    }
  }
#endif // NO_QA

  bl_init();
}
#else // TRMNL OG setup()
void setup() {

  bool testPassed = checkIfAlreadyPassed();
  if (!testPassed) {
    startQA();
  }
  esp_ota_mark_app_valid_cancel_rollback();
  bl_init();
}
#endif // !BOARD_TRMNL_X

void loop() { bl_process(); }

#endif // AVOLYN_HEADLESS_WIFI_TEST
