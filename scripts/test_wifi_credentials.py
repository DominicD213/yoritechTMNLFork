#!/usr/bin/env python3
"""Compile the firmware's actual credential codec/store contract with a fake NVS."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
json_include = Path(os.environ.get("ARDUINOJSON_INCLUDE_DIR", root / ".pio/libdeps/avolyn_headless/ArduinoJson/src"))
source = r'''
#include "avolyn_wifi_credentials.h"
#include <cassert>
#include <map>
#include <iostream>
struct Store {
  std::map<std::string, std::string> disk;
  bool fail = false, corrupt = false;
  bool isKey(const char *key) { return disk.count(key); }
  std::string getString(const char *key, const char *fallback) {
    return disk.count(key) ? disk[key] : fallback;
  }
  size_t putString(const char *key, const char *value) {
    if (fail) return 0;
    disk[key] = corrupt ? "broken" : value;
    return std::string(value).size();
  }
};
int main() {
  using namespace avolyn;
  Store store;
  WifiCredentials loaded;
  assert(loadWifiCredentials(store, loaded) == WifiLoad::Missing);
  store.disk = {{"ssid", "Test wifi"}, {"pass", "test-password"}};
  assert(loadWifiCredentials(store, loaded) == WifiLoad::Legacy);
  assert(saveWifiCredentials(store, loaded));
  Store rebooted; rebooted.disk = store.disk;
  WifiCredentials restored;
  assert(loadWifiCredentials(rebooted, restored) == WifiLoad::Saved);
  assert(restored.ssid == "Test wifi" && restored.password == "test-password");
  rebooted.fail = true;
  assert(!saveWifiCredentials(rebooted, {"Replacement", "new-password"}));
  assert(loadWifiCredentials(rebooted, restored) == WifiLoad::Saved);
  assert(restored.ssid == "Test wifi" && restored.password == "test-password");
  Store bad; bad.corrupt = true;
  assert(!saveWifiCredentials(bad, loaded));
  assert(loadWifiCredentials(bad, restored) == WifiLoad::Invalid);
  assert(saveWifiCredentials(store, {"Open network", ""}));
  assert(loadWifiCredentials(store, restored) == WifiLoad::Saved && restored.password.empty());
  assert(!saveWifiCredentials(store, {"", "test-password"}));
  assert(!saveWifiCredentials(store, {std::string(33, 'x'), "test-password"}));
  assert(!saveWifiCredentials(store, {"Network", "short"}));
  assert(!saveWifiCredentials(store, {"Network", std::string(64, 'x')}));
  store.disk["record"] = "{\"v\":1,\"ssid\":\"Network\",\"password\":2}";
  assert(loadWifiCredentials(store, restored) == WifiLoad::Invalid);
  store.disk["record"] = std::string(513, 'x');
  assert(loadWifiCredentials(store, restored) == WifiLoad::Invalid);
  std::cout << "Credential persistence, migration, power-cycle and write-failure tests passed.\n";
}
'''
if not json_include.is_dir():
    raise SystemExit("Run pio pkg install -e avolyn_headless first (ArduinoJson headers required).")
with tempfile.TemporaryDirectory(prefix="avolyn-credentials-") as folder:
    cpp = Path(folder) / "credentials.cpp"
    exe = Path(folder) / "credentials"
    cpp.write_text(source)
    subprocess.run(
        [os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
         *shlex.split(os.environ.get("CXXFLAGS", "")), "-I" + str(root / "include"),
         "-I" + str(json_include), str(cpp), "-o", str(exe)], check=True
    )
    subprocess.run([str(exe)], check=True)
