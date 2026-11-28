#pragma once

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <chrono>
#include <cstdint>
#ifndef MILLIS_DECLARED
#define MILLIS_DECLARED
inline uint32_t millis() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}
#endif
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>

class ReadingSpeedTracker {
 public:
  static constexpr size_t HISTORY_SIZE = 8;
  static constexpr uint32_t MIN_PAGE_TIME_MS = 2000;    // < 2s is rapid skimming
  static constexpr uint32_t MAX_PAGE_TIME_MS = 300000;  // > 5m is idle pause
  static constexpr uint32_t DEFAULT_SEC_PER_PAGE = 45;  // ~200-250 WPM baseline

  void onPageEntered(uint32_t now = millis()) {
    lastPageEnterTimeMs = now;
    hasPageEnter = true;
  }

  void onPageTurned(uint32_t now = millis()) {
    if (hasPageEnter) {
      const uint32_t delta = now - lastPageEnterTimeMs;
      if (delta >= MIN_PAGE_TIME_MS && delta <= MAX_PAGE_TIME_MS) {
        samples[sampleIndex] = delta / 1000;
        sampleIndex = (sampleIndex + 1) % HISTORY_SIZE;
        if (sampleCount < HISTORY_SIZE) {
          sampleCount++;
        }
      }
    }
    lastPageEnterTimeMs = now;
    hasPageEnter = true;
  }

  uint32_t getAverageSecondsPerPage() const {
    if (sampleCount == 0) {
      return DEFAULT_SEC_PER_PAGE;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
      sum += samples[i];
    }
    return sum / sampleCount;
  }

  int getMinutesLeftInChapter(int currentPage, int totalPagesInChapter) const {
    if (totalPagesInChapter <= currentPage) {
      return 0;
    }
    const int pagesLeft = totalPagesInChapter - currentPage;
    const uint32_t secPerPage = getAverageSecondsPerPage();
    const uint32_t totalSec = pagesLeft * secPerPage;
    return std::max(1, static_cast<int>((totalSec + 30) / 60));
  }

  int getMinutesLeftInBook(float bookProgressPercent, int currentPage, int totalPagesInChapter,
                           float chapterProgressShare = 0.0f) const {
    if (bookProgressPercent >= 99.5f) {
      return 0;
    }
    const uint32_t secPerPage = getAverageSecondsPerPage();
    const int minInChapter = getMinutesLeftInChapter(currentPage, totalPagesInChapter);
    const float remainingFraction = (100.0f - std::clamp(bookProgressPercent, 0.0f, 100.0f)) / 100.0f;
    float estimatedTotalPages = 250.0f;
    if (totalPagesInChapter > 0 && chapterProgressShare > 0.001f) {
      estimatedTotalPages = static_cast<float>(totalPagesInChapter) / chapterProgressShare;
    } else if (bookProgressPercent < 1.0f) {
      return std::max(1, minInChapter * 10);
    } else if (totalPagesInChapter > 0 && bookProgressPercent > 0.0f) {
      const int effectivePages = std::max(currentPage, totalPagesInChapter);
      estimatedTotalPages = static_cast<float>(effectivePages) / (bookProgressPercent / 100.0f);
    }
    const float minPagesLeft = static_cast<float>(std::max(0, totalPagesInChapter - currentPage));
    const float remainingPages = std::max(minPagesLeft, estimatedTotalPages * remainingFraction);
    const uint32_t totalSec = static_cast<uint32_t>(remainingPages * secPerPage);
    return std::max({1, minInChapter, static_cast<int>((totalSec + 30) / 60)});
  }

  static void formatTimeLeft(char* buffer, size_t bufferSize, int minutes, const char* mSuffix = "m",
                             const char* hSuffix = "h") {
    if (!buffer || bufferSize == 0) return;
    if (minutes <= 0) {
      buffer[0] = '\0';
      return;
    }
    if (minutes < 60) {
      snprintf(buffer, bufferSize, "%d%s", minutes, mSuffix ? mSuffix : "m");
    } else {
      const int hours = minutes / 60;
      const int remainingMins = minutes % 60;
      if (remainingMins == 0) {
        snprintf(buffer, bufferSize, "%d%s", hours, hSuffix ? hSuffix : "h");
      } else {
        snprintf(buffer, bufferSize, "%d%s %d%s", hours, hSuffix ? hSuffix : "h", remainingMins,
                 mSuffix ? mSuffix : "m");
      }
    }
  }

 private:
  uint32_t lastPageEnterTimeMs = 0;
  bool hasPageEnter = false;
  uint32_t samples[HISTORY_SIZE] = {0};
  size_t sampleIndex = 0;
  size_t sampleCount = 0;
};
