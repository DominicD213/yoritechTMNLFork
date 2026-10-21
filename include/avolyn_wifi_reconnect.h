#pragma once

#include <stdint.h>

namespace avolyn {
  constexpr uint8_t MAX_SAVED_WIFI_ATTEMPTS = 3;

  inline bool shouldRetrySavedWifi(bool online, bool connecting, bool saved, uint32_t elapsed, uint32_t retryAfter,
                                   uint8_t attempts) {
    return saved && !online && !connecting && attempts < MAX_SAVED_WIFI_ATTEMPTS && elapsed >= retryAfter;
  }
} // namespace avolyn
