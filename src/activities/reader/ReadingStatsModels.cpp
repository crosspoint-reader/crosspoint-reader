#include <algorithm>
#include <cstdio>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) return;
  if (seconds > UINT16_MAX) seconds = UINT16_MAX;
  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }

  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    const uint32_t weight = paceSampleCount;
    avgSecondsPerForwardPage =
        static_cast<uint16_t>((static_cast<uint64_t>(avgSecondsPerForwardPage) * weight + sample) / (weight + 1u));
    ++paceSampleCount;
    return;
  }

  // Once the persisted sample count reaches its cap, retain a bounded moving
  // average. Integer division toward zero would otherwise make higher samples
  // stick forever while lower samples still pull the average down by one.
  const int32_t delta = static_cast<int32_t>(sample) - avgSecondsPerForwardPage;
  if (delta == 0) return;
  int32_t adjustment = delta / static_cast<int32_t>(MAX_PACE_SAMPLE_COUNT + 1u);
  if (adjustment == 0) adjustment = delta > 0 ? 1 : -1;
  avgSecondsPerForwardPage = static_cast<uint16_t>(static_cast<int32_t>(avgSecondsPerForwardPage) + adjustment);
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(const uint32_t seconds, char* buffer, const size_t length) {
  if (!buffer || length == 0) return;
  if (seconds < 60) {
    snprintf(buffer, length, "< 1 min");
    return;
  }
  const uint32_t hours = seconds / 3600u;
  const uint32_t minutes = seconds % 3600u / 60u;
  if (hours == 0) {
    snprintf(buffer, length, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buffer, length, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

void GlobalReadingStats::merge(const GlobalReadingStats& other) {
  totalSessions = addReadingStatsSaturated(totalSessions, other.totalSessions);
  totalReadingSeconds = addReadingStatsSaturated(totalReadingSeconds, other.totalReadingSeconds);
  totalPagesTurned = addReadingStatsSaturated(totalPagesTurned, other.totalPagesTurned);
  completedBooks = addReadingStatsSaturated(completedBooks, other.completedBooks);
  for (size_t i = 0; i < timeOfDaySeconds.size(); ++i) {
    timeOfDaySeconds[i] = addReadingStatsSaturated(timeOfDaySeconds[i], other.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < dayOfWeekSeconds.size(); ++i) {
    dayOfWeekSeconds[i] = addReadingStatsSaturated(dayOfWeekSeconds[i], other.dayOfWeekSeconds[i]);
  }
  mergeReadingHistory(readingHistoryAnchorDay, readingHistoryBits, other.readingHistoryAnchorDay,
                      other.readingHistoryBits);
  longestReadingStreak = std::max(longestReadingStreak, other.longestReadingStreak);
}

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);
  longestReadingStreak =
      std::max(longestReadingStreak, computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}

uint16_t GlobalReadingStats::displayLongestReadingStreak() const {
  return std::max(longestReadingStreak,
                  computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}
