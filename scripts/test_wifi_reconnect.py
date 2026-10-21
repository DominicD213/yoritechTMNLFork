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
  assert(!shouldRetrySavedWifi(false, false, false, 50000, delay, 0)); // No credentials.
  assert(!shouldRetrySavedWifi(false, true, true, 50000, delay, 1));  // In progress.
  assert(!shouldRetrySavedWifi(true, false, true, 50000, delay, 2));  // Connected.
  assert(!shouldRetrySavedWifi(false, false, true, 34999, delay, 1)); // Backoff active.
  assert(shouldRetrySavedWifi(false, false, true, 35000, delay, 0));  // Initial retry.
  assert(shouldRetrySavedWifi(false, false, true, 35000, delay, 1));  // Second attempt.
  assert(shouldRetrySavedWifi(false, false, true, 35000, delay, 2));  // Third attempt.
  assert(!shouldRetrySavedWifi(false, false, true, 60000, delay, 3)); // Exhausted.
  assert(!shouldRetrySavedWifi(false, false, true, 60000, delay, 255));
  assert(shouldRetrySavedWifi(false, false, true, 35000, delay, 0));  // Manual reset.
  // Unsigned millis() subtraction must stay valid after rollover.
  const uint32_t previous = UINT32_MAX - 1000;
  assert(!shouldRetrySavedWifi(false, false, true, uint32_t(32998U - previous), delay, 2));
  assert(shouldRetrySavedWifi(false, false, true, uint32_t(33999U - previous), delay, 2));
  std::cout << "Bounded Wi-Fi retries and timer rollover tests passed.\\n";
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
