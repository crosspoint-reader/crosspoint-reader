#include "ReadingStatsPresentation.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace {
template <size_t N>
ReadingStatsChart<N> buildChart(const std::array<uint32_t, N>& values, const uint32_t totalReadingSeconds,
                                const bool trusted) {
  ReadingStatsChart<N> chart;
  chart.seconds = values;
  if (!trusted) return chart;

  const uint64_t representedSeconds = std::accumulate(values.begin(), values.end(), uint64_t{0}, std::plus<uint64_t>{});

  // A real zero is known. A non-zero legacy total with no dated buckets is
  // not an empty chart: its distribution is unavailable.
  chart.available = totalReadingSeconds == 0 || representedSeconds > 0;
  chart.incomplete = chart.available && representedSeconds < totalReadingSeconds;
  return chart;
}

BookReadingStatsPresentation buildBookPresentation(const BookReadingStats& stats, const bool trusted,
                                                   const ReadingStatsDateTime* now, const ReadingStatsMetric progress,
                                                   const bool hasFreshTimeEstimate) {
  BookReadingStatsPresentation model;
  model.progress = progress;
  if (!trusted) return model;

  model.readingTime = ReadingStatsMetric::known(stats.totalReadingSeconds);
  model.sessions = ReadingStatsMetric::known(stats.sessionCount);
  model.pagesTurned = ReadingStatsMetric::known(stats.totalPagesTurned);
  model.completed = ReadingStatsMetric::known(stats.isCompleted ? 1u : 0u);
  // totalReadingSeconds includes visits from ten seconds onward, while
  // sessionCount includes only visits lasting at least sixty seconds. The
  // persisted schema cannot reconstruct an honest average session duration.
  if (stats.paceSampleCount > 0 && stats.avgSecondsPerForwardPage > 0) {
    model.averagePage = ReadingStatsMetric::known(stats.avgSecondsPerForwardPage);
  }
  if (stats.isCompleted) {
    model.timeLeft = ReadingStatsMetric::known(0);
  } else if (hasFreshTimeEstimate && stats.estimatedTimeLeftSeconds > 0) {
    model.timeLeft = ReadingStatsMetric::estimated(stats.estimatedTimeLeftSeconds);
  }

  const bool hasValidNow = now && now->isValid();
  const auto dateIsNotFuture = [hasValidNow, now](const ReadingStatsDate& date) {
    return date.isValid() && (!hasValidNow || compareReadingStatsDate(date, now->date) <= 0);
  };
  if (dateIsNotFuture(stats.startDate)) {
    model.startDate = ReadingStatsMetric::known(readingStatsDayIndex(stats.startDate));
  }
  if (stats.isCompleted) {
    if (dateIsNotFuture(stats.finishedDate)) {
      model.finishDate = ReadingStatsMetric::known(readingStatsDayIndex(stats.finishedDate));
    }
  } else if (hasValidNow && hasFreshTimeEstimate && stats.startDate.isValid() &&
             compareReadingStatsDate(stats.startDate, now->date) <= 0 && stats.totalReadingSeconds > 0 &&
             stats.estimatedTimeLeftSeconds > 0) {
    const uint64_t elapsedDays = std::max<uint16_t>(1, readingSpanDaysElapsed(stats.startDate, now->date));
    constexpr uint64_t SECONDS_PER_DAY = 24u * 60u * 60u;
    const auto saturatedMultiply = [](const uint64_t lhs, const uint64_t rhs) {
      const uint64_t maximum = std::numeric_limits<uint64_t>::max();
      return lhs != 0 && rhs > maximum / lhs ? maximum : lhs * rhs;
    };
    const auto saturatedAdd = [](const uint64_t lhs, const uint64_t rhs) {
      const uint64_t maximum = std::numeric_limits<uint64_t>::max();
      return maximum - lhs < rhs ? maximum : lhs + rhs;
    };
    uint64_t calendarSeconds = saturatedMultiply(stats.estimatedTimeLeftSeconds, elapsedDays);
    calendarSeconds = saturatedMultiply(calendarSeconds, SECONDS_PER_DAY);
    calendarSeconds = saturatedAdd(calendarSeconds, stats.totalReadingSeconds / 2u) / stats.totalReadingSeconds;

    const uint64_t currentSeconds = static_cast<uint64_t>(readingStatsDayIndex(now->date)) * SECONDS_PER_DAY +
                                    static_cast<uint32_t>(now->hour) * 3600u +
                                    static_cast<uint32_t>(now->minute) * 60u + now->second;
    const uint64_t lastSeconds =
        (static_cast<uint64_t>(readingStatsDayIndex({2099, 12, 31})) + 1u) * SECONDS_PER_DAY - 1u;
    if (calendarSeconds > 0 && calendarSeconds <= lastSeconds - currentSeconds) {
      ReadingStatsDateTime estimate = *now;
      addSecondsToReadingStatsDateTime(estimate, static_cast<uint32_t>(calendarSeconds));
      model.finishDate = ReadingStatsMetric::estimated(readingStatsDayIndex(estimate.date));
    }
  }
  model.timeOfDay = buildChart(stats.timeOfDaySeconds, stats.totalReadingSeconds, true);
  model.dayOfWeek = buildChart(stats.dayOfWeekSeconds, stats.totalReadingSeconds, true);
  return model;
}

GlobalReadingStatsPresentation buildGlobalPresentation(const GlobalReadingStats& stats, const bool trusted,
                                                       const ReadingStatsDate* today) {
  GlobalReadingStatsPresentation model;
  if (!trusted) return model;

  model.readingTime = ReadingStatsMetric::known(stats.totalReadingSeconds);
  model.sessions = ReadingStatsMetric::known(stats.totalSessions);
  model.pagesTurned = ReadingStatsMetric::known(stats.totalPagesTurned);
  model.completedBooks = ReadingStatsMetric::known(stats.completedBooks);
  // See the per-book explanation above: the two cumulative fields use
  // different duration thresholds, so their ratio is not an average session.
  if (today && today->isValid()) {
    model.currentStreak = ReadingStatsMetric::known(stats.currentReadingStreak(today));
  }
  uint32_t readingDays = 0;
  for (size_t index = 0; index < READING_HISTORY_DAYS; ++index) {
    if ((stats.readingHistoryBits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0) ++readingDays;
  }
  const bool hasDatedHistory = readingDays > 0 || stats.longestReadingStreak > 0;
  if (stats.totalReadingSeconds == 0 || hasDatedHistory) {
    model.readingDays = ReadingStatsMetric::known(readingDays);
    model.longestStreak = ReadingStatsMetric::known(stats.displayLongestReadingStreak());
  }
  model.timeOfDay = buildChart(stats.timeOfDaySeconds, stats.totalReadingSeconds, true);
  model.dayOfWeek = buildChart(stats.dayOfWeekSeconds, stats.totalReadingSeconds, true);
  return model;
}
}  // namespace

ReadingStatsPresentation buildReadingStatsPresentation(
    const BookReadingStats& bookStats, const bool bookStatsTrusted, const GlobalReadingStats& deviceStats,
    const bool deviceStatsTrusted, const GlobalReadingStatsAggregation& allSyncedStats, const ReadingStatsDateTime* now,
    const ReadingStatsMetric progress, const bool hasFreshTimeEstimate) {
  ReadingStatsPresentation model;
  model.book = buildBookPresentation(bookStats, bookStatsTrusted, now, progress, hasFreshTimeEstimate);
  const ReadingStatsDate* today = now && now->isValid() ? &now->date : nullptr;
  model.device = buildGlobalPresentation(deviceStats, deviceStatsTrusted, today);
  model.validPeerCount = allSyncedStats.validPeerCount;
  model.skippedPeerCount = allSyncedStats.skippedPeerCount;
  model.showAllSynced = allSyncedStats.validPeerCount > 0;
  model.syncAggregateComplete = deviceStatsTrusted && allSyncedStats.validPeerCount > 0 &&
                                allSyncedStats.skippedPeerCount == 0 && allSyncedStats.scanComplete;
  model.allSynced = buildGlobalPresentation(allSyncedStats.stats, model.syncAggregateComplete, today);
  // A synced snapshot identifies a device, not the books contributing to its
  // counter. Summing completedBooks can count the same book once per device.
  model.allSynced.completedBooks = ReadingStatsMetric::unavailable();
  return model;
}

int scaleReadingStatsBar(const uint32_t value, const uint32_t maximum, const int availableWidth) {
  if (value == 0 || maximum == 0 || availableWidth <= 0) return 0;
  const int scaled = static_cast<int>((static_cast<uint64_t>(value) * availableWidth) / maximum);
  return std::min(availableWidth, std::max(std::min(2, availableWidth), scaled));
}

void previewReadingStatsSession(BookReadingStats* bookStats, GlobalReadingStats* deviceStats, const uint32_t seconds,
                                const BookReadingStats& pendingBookSpans,
                                const GlobalReadingStats& pendingGlobalSpans) {
  if (seconds >= 10) {
    if (bookStats) {
      bookStats->totalReadingSeconds = addReadingStatsSaturated(bookStats->totalReadingSeconds, seconds);
      for (size_t index = 0; index < bookStats->timeOfDaySeconds.size(); ++index) {
        bookStats->timeOfDaySeconds[index] =
            addReadingStatsSaturated(bookStats->timeOfDaySeconds[index], pendingBookSpans.timeOfDaySeconds[index]);
      }
      for (size_t index = 0; index < bookStats->dayOfWeekSeconds.size(); ++index) {
        bookStats->dayOfWeekSeconds[index] =
            addReadingStatsSaturated(bookStats->dayOfWeekSeconds[index], pendingBookSpans.dayOfWeekSeconds[index]);
      }
    }
    if (deviceStats) {
      deviceStats->totalReadingSeconds = addReadingStatsSaturated(deviceStats->totalReadingSeconds, seconds);
      deviceStats->merge(pendingGlobalSpans);
    }
  }

  if (seconds < 60) return;
  if (bookStats && bookStats->sessionCount < UINT16_MAX) ++bookStats->sessionCount;
  if (deviceStats) deviceStats->totalSessions = addReadingStatsSaturated(deviceStats->totalSessions, 1);
}
