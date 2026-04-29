#pragma once

#include <Arduino.h>

#include <cmath>
#include <cstdint>
#include <optional>

class ReadingEtaTracker {
 public:
  uint32_t getAvgMsPerPage() const {
    if (validSamples < minSamplesForEta || emaSecondsPerPage <= 0.0f) return 0;
    const float ms = emaSecondsPerPage * 1000.0f;
    return static_cast<uint32_t>(ms + 0.5f);
  }

  void restoreAvgMsPerPage(const uint32_t avgMs) {
    // sanity: не больше 10 минут на страницу
    if (avgMs == 0 || avgMs > maxIntervalMs) return;
    emaSecondsPerPage = static_cast<float>(avgMs) / 1000.0f;
    validSamples = minSamplesForEta;  // чтобы ETA показывался сразу
    // Важно: пауза сна не должна стать "временем чтения страницы"
    hasLastPage = false;
    lastPageKey = 0;
    lastPageTurnTimeMs = 0;
  }

  void resetTimingAfterSleep() {
    hasLastPage = false;
    lastPageKey = 0;
    lastPageTurnTimeMs = 0;
  }
  std::optional<int> updateAndGetMinutes(const int sectionIndex, const int pageNumber, const int remainingPages) {
    const uint32_t now = millis();
    const uint32_t pageKey = makePageKey(sectionIndex, pageNumber);

    // Ignore duplicate renders for the same page key.
    if (hasLastPage && pageKey == lastPageKey) {
      return estimateMinutes(remainingPages);
    }

    if (hasLastPage) {
      const uint32_t intervalMs = now - lastPageTurnTimeMs;
      if (intervalMs >= minIntervalMs && intervalMs <= maxIntervalMs) {
        const float sampleSecondsPerPage = static_cast<float>(intervalMs) / 1000.0f;
        if (validSamples == 0) {
          emaSecondsPerPage = sampleSecondsPerPage;
        } else {
          emaSecondsPerPage = (emaAlpha * sampleSecondsPerPage) + ((1.0f - emaAlpha) * emaSecondsPerPage);
        }
        validSamples++;
      }
    }

    hasLastPage = true;
    lastPageKey = pageKey;
    lastPageTurnTimeMs = now;
    return estimateMinutes(remainingPages);
  }

 private:
  static constexpr float emaAlpha = 0.125f;
  static constexpr uint32_t minIntervalMs = 800;
  static constexpr uint32_t maxIntervalMs = 10UL * 60UL * 1000UL;
  static constexpr uint8_t minSamplesForEta = 2;

  bool hasLastPage = false;
  uint32_t lastPageKey = 0;
  uint32_t lastPageTurnTimeMs = 0;
  uint8_t validSamples = 0;
  float emaSecondsPerPage = 0.0f;

  static uint32_t makePageKey(const int sectionIndex, const int pageNumber) {
    return (static_cast<uint32_t>(sectionIndex & 0xFFFF) << 16) | static_cast<uint32_t>(pageNumber & 0xFFFF);
  }

  std::optional<int> estimateMinutes(const int remainingPages) const {
    if (remainingPages <= 0) {
      return 0;
    }
    if (validSamples < minSamplesForEta || emaSecondsPerPage <= 0.0f) {
      return std::nullopt;
    }

    const float totalMinutes = (static_cast<float>(remainingPages) * emaSecondsPerPage) / 60.0f;
    return static_cast<int>(std::ceil(totalMinutes));
  }
};
