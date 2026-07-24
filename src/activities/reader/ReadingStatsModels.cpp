#include <algorithm>
#include <cstdio>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

namespace {
bool historyContainsDay(const GlobalReadingStats& stats, const uint32_t day) {
  if (stats.readingHistoryAnchorDay < day) return false;
  const uint32_t delta = stats.readingHistoryAnchorDay - day;
  return delta < READING_HISTORY_DAYS &&
         (stats.readingHistoryBits[delta / 8] & static_cast<uint8_t>(1u << (delta % 8))) != 0;
}

void recordLatestDay(GlobalReadingStats& stats, const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) return;
  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint32_t day = readingStatsDayIndex(cursor.date);
    const uint32_t secondOfDay =
        static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u + cursor.second;
    const uint32_t segment = std::min(remaining, 24u * 3600u - secondOfDay);
    if (!stats.hasLatestDayReadingSeconds ||
        (day > stats.latestReadingDay && day - stats.latestReadingDay < READING_HISTORY_DAYS)) {
      stats.latestReadingDay = day;
      stats.latestDayReadingSeconds = segment;
      stats.latestDaySessions = 0;
      stats.hasLatestDayReadingSeconds = true;
    } else if (day == stats.latestReadingDay) {
      stats.latestDayReadingSeconds = addReadingStatsSaturated(stats.latestDayReadingSeconds, segment);
    }
    remaining -= segment;
    const uint32_t previousDay = readingStatsDayIndex(cursor.date);
    addSecondsToReadingStatsDateTime(cursor, segment);
    if (remaining > 0 && previousDay == readingStatsDayIndex(cursor.date)) break;
  }
}
}  // namespace

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
  const bool targetAlreadyHadLatestDay =
      other.hasLatestDayReadingSeconds && historyContainsDay(*this, other.latestReadingDay);
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
  if (other.hasLatestDayReadingSeconds) {
    if (hasLatestDayReadingSeconds && latestReadingDay == other.latestReadingDay) {
      latestDayReadingSeconds = addReadingStatsSaturated(latestDayReadingSeconds, other.latestDayReadingSeconds);
      latestDaySessions = addReadingStatsSaturated(latestDaySessions, other.latestDaySessions);
    } else {
      const uint32_t knownLatestDay = hasLatestDayReadingSeconds ? latestReadingDay : readingHistoryAnchorDay;
      const bool plausibleNewerDay = knownLatestDay == 0 || other.latestReadingDay <= knownLatestDay ||
                                     other.latestReadingDay - knownLatestDay < READING_HISTORY_DAYS;
      if (((!hasLatestDayReadingSeconds && !targetAlreadyHadLatestDay) ||
          (hasLatestDayReadingSeconds && other.latestReadingDay > latestReadingDay)) &&
          plausibleNewerDay) {
        latestReadingDay = other.latestReadingDay;
        latestDayReadingSeconds = other.latestDayReadingSeconds;
        latestDaySessions = other.latestDaySessions;
        hasLatestDayReadingSeconds = true;
      }
    }
  }
}

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordLatestDay(*this, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);
  longestReadingStreak =
      std::max(longestReadingStreak, computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}

void GlobalReadingStats::recordReadingSession(const ReadingStatsDate& localDate) {
  if (!localDate.isValid()) return;
  const uint32_t day = readingStatsDayIndex(localDate);
  if (hasLatestDayReadingSeconds && latestReadingDay == day) {
    latestDaySessions = addReadingStatsSaturated(latestDaySessions, 1);
    return;
  }

  // If the legacy rolling history already contains this day, its exact
  // session count is unknowable. Keep the summary unknown instead of showing
  // only the newly recorded session as if it were the whole day.
  if (historyContainsDay(*this, day)) return;

  const uint32_t knownLatestDay = hasLatestDayReadingSeconds ? latestReadingDay : readingHistoryAnchorDay;
  const bool plausibleDay = knownLatestDay == 0 || day <= knownLatestDay || day - knownLatestDay < READING_HISTORY_DAYS;
  if (!plausibleDay || (hasLatestDayReadingSeconds && day < latestReadingDay)) return;

  latestReadingDay = day;
  latestDayReadingSeconds = 0;
  latestDaySessions = 1;
  hasLatestDayReadingSeconds = true;
}

bool GlobalReadingStats::readingSecondsForDate(const ReadingStatsDate& date, uint32_t& seconds) const {
  uint32_t sessions = 0;
  return readingSummaryForDate(date, seconds, sessions);
}

bool GlobalReadingStats::readingSummaryForDate(const ReadingStatsDate& date, uint32_t& seconds,
                                               uint32_t& sessions) const {
  if (!date.isValid()) return false;
  const uint32_t day = readingStatsDayIndex(date);
  if (hasLatestDayReadingSeconds && latestReadingDay == day) {
    seconds = latestDayReadingSeconds;
    sessions = latestDaySessions;
    return true;
  }
  // A fresh store is exactly zero. Once the sidecar has established the last
  // fully attributed local day, a later unmarked day is also exactly zero.
  // Legacy totals without a sidecar may include reading recorded while the
  // clock was invalid, so absence from their history is not proof of zero.
  const bool afterKnownDailySummary = hasLatestDayReadingSeconds && day > latestReadingDay &&
                                      day - latestReadingDay < READING_HISTORY_DAYS;
  const bool pristineStats = totalReadingSeconds == 0 && totalSessions == 0 && readingHistoryAnchorDay == 0 &&
                             !historyContainsDay(*this, day);
  if (pristineStats || (afterKnownDailySummary && !historyContainsDay(*this, day))) {
    seconds = 0;
    sessions = 0;
    return true;
  }
  return false;
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}

uint16_t GlobalReadingStats::displayLongestReadingStreak() const {
  return std::max(longestReadingStreak,
                  computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}
