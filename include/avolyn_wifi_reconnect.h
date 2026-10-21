#pragma once

#include <stdint.h>

namespace avolyn {
  inline bool shouldRetrySavedWifi(bool online, bool connecting, bool saved, uint32_t elapsed, uint32_t retryAfter) {
    return saved && !online && !connecting && elapsed >= retryAfter;
  }
} // namespace avolyn
