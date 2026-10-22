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
  using avolyn::wifiRetryDelayMs;
  assert(wifiRetryDelayMs(0) == 2000U);
  assert(wifiRetryDelayMs(1) == 2000U);
  assert(wifiRetryDelayMs(2) == 4000U);
  assert(wifiRetryDelayMs(3) == 8000U);
  assert(wifiRetryDelayMs(4) == 16000U);
  assert(wifiRetryDelayMs(5) == 30000U);
  assert(wifiRetryDelayMs(255) == 30000U);
  assert(!shouldRetrySavedWifi(false, false, false, 999999, 2000)); // No saved credentials.
  assert(!shouldRetrySavedWifi(true, false, true, 999999, 2000)); // Already online.
  assert(!shouldRetrySavedWifi(false, true, true, 999999, 2000)); // Attempt in progress.
  assert(!shouldRetrySavedWifi(false, false, true, 1999, 2000)); // Respect backoff.
  assert(shouldRetrySavedWifi(false, false, true, 2000, 2000));
  assert(shouldRetrySavedWifi(false, false, true, 30000, wifiRetryDelayMs(255)));
  // Unsigned millis() subtraction works after the 32-bit timer wraps.
  const uint32_t previous = UINT32_MAX - 1000;
  assert(!shouldRetrySavedWifi(false, false, true, uint32_t(998U - previous), 2000));
  assert(shouldRetrySavedWifi(false, false, true, uint32_t(999U - previous), 2000));
  assert(!avolyn::wifiAttemptExpired(15000, false, 0));
  assert(!avolyn::wifiAttemptExpired(29999, false, 0));
  assert(avolyn::wifiAttemptExpired(30000, false, 0));
  assert(!avolyn::wifiAttemptExpired(31000, true, 2000));
  assert(avolyn::wifiAttemptExpired(31000, true, 15000));
  assert(avolyn::wifiAttemptExpired(45000, true, 1000));
  std::cout << "Persistent Wi-Fi backoff and rollover tests passed.\\n";
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
