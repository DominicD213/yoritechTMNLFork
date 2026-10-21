#pragma once

#include <stdint.h>

namespace avolyn {
  // Retry quickly after a temporary outage, then at most once every 30 seconds.
  // There is intentionally no attempt cap on the device: the router can return
  // hours later, with no computer/USB cable available to rescue the Slate.
  constexpr uint32_t WIFI_RETRY_INITIAL_MS = 2000;
  constexpr uint32_t WIFI_RETRY_MAX_MS = 30000;

  inline uint32_t wifiRetryDelayMs(uint8_t consecutiveFailures) {
    uint32_t delay = WIFI_RETRY_INITIAL_MS;
    for (uint8_t i = 1; i < consecutiveFailures && delay < WIFI_RETRY_MAX_MS; ++i) {
      delay *= 2;
      if (delay > WIFI_RETRY_MAX_MS) delay = WIFI_RETRY_MAX_MS;
    }
    return delay;
  }

  inline bool shouldRetrySavedWifi(bool online, bool connecting, bool hasSavedCredentials, uint32_t elapsed,
                                   uint32_t delayMs) {
    return hasSavedCredentials && !online && !connecting && elapsed >= delayMs;
  }
} // namespace avolyn
