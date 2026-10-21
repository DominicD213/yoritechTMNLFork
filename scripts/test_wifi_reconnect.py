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
  assert(!shouldRetrySavedWifi(false, false, false, 50000, 0)); // No saved credentials.
  assert(!shouldRetrySavedWifi(false, true, true, 50000, 0)); // An attempt is in progress.
  assert(!shouldRetrySavedWifi(true, false, true, 50000, 0)); // Already joined.
  assert(!shouldRetrySavedWifi(false, false, true, 34999, 0)); // Backoff not finished.
  assert(shouldRetrySavedWifi(false, false, true, 35000, 0));
  assert(!shouldRetrySavedWifi(false, false, true, 55000, 25000));
  assert(shouldRetrySavedWifi(false, false, true, 60000, 25000));
  // millis() wraps after long-running device uptime.
  const uint32_t previous = UINT32_MAX - 1000;
  assert(!shouldRetrySavedWifi(false, false, true, 32998, previous));
  assert(shouldRetrySavedWifi(false, false, true, 33999, previous));
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
