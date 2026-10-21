#pragma once

#include <stdint.h>

namespace avolyn {
  // The initial connection has 20 seconds; retry saved credentials only after
  // a separate 15-second backoff. Wrap-safe for Arduino millis() rollover.
  inline bool shouldRetrySavedWifi(bool connected, bool connecting, bool hasSavedNetwork,
                                   uint32_t now, uint32_t lastAttemptAt,
                                   uint32_t connectTimeoutMs = 20000U,
                                   uint32_t backoffMs = 15000U) {
    return hasSavedNetwork && !connected && !connecting &&
           uint32_t(now - lastAttemptAt) >= connectTimeoutMs + backoffMs;
  }
}
