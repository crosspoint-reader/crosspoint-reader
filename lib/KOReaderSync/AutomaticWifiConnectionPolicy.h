#pragma once

#include <algorithm>
#include <cstdint>

class AutomaticWifiConnectionPolicy {
 public:
  static constexpr uint32_t BACKGROUND_TIMEOUT_MS = 15000;
  static constexpr uint32_t FALLBACK_ATTEMPT_TIMEOUT_MS = 6000;

  static bool backgroundDeadlineExpired(const uint32_t now, const uint32_t startedAt) {
    return remainingBackgroundTimeMs(now, startedAt) == 0;
  }

  static uint32_t remainingBackgroundTimeMs(const uint32_t now, const uint32_t startedAt) {
    const uint32_t elapsed = static_cast<uint32_t>(now - startedAt);
    return elapsed >= BACKGROUND_TIMEOUT_MS ? 0 : BACKGROUND_TIMEOUT_MS - elapsed;
  }

  static uint32_t connectionAttemptTimeoutMs(const uint32_t now, const uint32_t startedAt,
                                             const bool hasFallbackNetwork) {
    const uint32_t remaining = remainingBackgroundTimeMs(now, startedAt);
    return hasFallbackNetwork ? std::min(remaining, FALLBACK_ATTEMPT_TIMEOUT_MS) : remaining;
  }
};
