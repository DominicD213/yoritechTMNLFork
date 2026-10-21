#!/usr/bin/env python3
"""Host-only checks for reboot Wi-Fi reconnection scheduling; never accesses hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include "avolyn_wifi_reconnect.h"
#include <cassert>
#include <cstdint>
#include <iostream>
int main() {
  using avolyn::shouldRetrySavedWifi;
  constexpr uint32_t delay = 35000U;
  assert(!shouldRetrySavedWifi(false, false, false, 50000, delay)); // No saved credentials.
  assert(!shouldRetrySavedWifi(false, true, true, 50000, delay)); // Attempt in progress.
  assert(!shouldRetrySavedWifi(true, false, true, 50000, delay)); // Already joined.
  assert(!shouldRetrySavedWifi(false, false, true, 34999, delay)); // Backoff not finished.
  assert(shouldRetrySavedWifi(false, false, true, 35000, delay));
  assert(!shouldRetrySavedWifi(false, false, true, 55000U - 25000U, delay));
  assert(shouldRetrySavedWifi(false, false, true, 60000U - 25000U, delay));
  // ESP32 millis() wraps, but elapsed subtraction remains correct using uint32_t.
  const uint32_t previous = UINT32_MAX - 1000;
  assert(!shouldRetrySavedWifi(false, false, true, uint32_t(32998U - previous), delay));
  assert(shouldRetrySavedWifi(false, false, true, uint32_t(33999U - previous), delay));
  std::cout << "Saved Wi-Fi retry scheduling and timer wrap tests passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix="avolyn-wifi-") as folder:
    cpp = Path(folder) / "wifi_retry.cpp"
    exe = Path(folder) / "wifi_retry"
    cpp.write_text(source)
    subprocess.run(
        [os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
         *shlex.split(os.environ.get("CXXFLAGS", "")), "-I" + str(root / "include"), str(cpp), "-o", str(exe)],
        check=True
    )
    subprocess.run([str(exe)], check=True)
