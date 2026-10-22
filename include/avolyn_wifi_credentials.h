#pragma once

#include <ArduinoJson.h>
#include <string>

namespace avolyn {
  struct WifiCredentials {
    std::string ssid;
    std::string password;
  };
  enum class WifiLoad { Missing, Legacy, Saved, Invalid };

  inline bool validWifiCredentials(const WifiCredentials &value) {
    return !value.ssid.empty() && value.ssid.size() <= 32 && value.password.size() <= 63 &&
           (value.password.empty() || value.password.size() >= 8) && value.ssid.find('\0') == std::string::npos &&
           value.password.find('\0') == std::string::npos;
  }

  template <typename Store> WifiLoad loadWifiCredentials(Store &store, WifiCredentials &value) {
    if (store.isKey("record")) {
      const auto record = store.getString("record", "");
      if (record.length() > 512) return WifiLoad::Invalid;
      JsonDocument json;
      if (deserializeJson(json, record.c_str()) || !json["v"].template is<int>() || json["v"].template as<int>() != 1 ||
          !json["ssid"].template is<std::string>() || !json["password"].template is<std::string>())
        return WifiLoad::Invalid;
      value = {json["ssid"].template as<std::string>(), json["password"].template as<std::string>()};
      return validWifiCredentials(value) ? WifiLoad::Saved : WifiLoad::Invalid;
    }
    value = {store.getString("ssid", "").c_str(), store.getString("pass", "").c_str()};
    if (value.ssid.empty()) return WifiLoad::Missing;
    return validWifiCredentials(value) ? WifiLoad::Legacy : WifiLoad::Invalid;
  }

  template <typename Store> bool saveWifiCredentials(Store &store, const WifiCredentials &value) {
    if (!validWifiCredentials(value)) return false;
    JsonDocument json;
    json["v"] = 1;
    json["ssid"] = value.ssid;
    json["password"] = value.password;
    std::string record;
    serializeJson(json, record);
    // One committed NVS value prevents mixing SSID/password across power loss.
    if (store.putString("record", record.c_str()) != record.size()) return false;
    return record == store.getString("record", "").c_str();
  }
} // namespace avolyn
