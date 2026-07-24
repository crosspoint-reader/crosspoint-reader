#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingCalendarModel.h"

enum class ReadingStatsMetricState : uint8_t { Known, Estimated, Unavailable, NotApplicable, NoData };

struct ReadingStatsMetric {
  ReadingStatsMetricState state = ReadingStatsMetricState::Unavailable;
  uint32_t value = 0;

  static constexpr ReadingStatsMetric known(const uint32_t value) { return {ReadingStatsMetricState::Known, value}; }
  static constexpr ReadingStatsMetric estimated(const uint32_t value) {
    return {ReadingStatsMetricState::Estimated, value};
  }
  static constexpr ReadingStatsMetric unavailable() { return {}; }
  static constexpr ReadingStatsMetric notApplicable() { return {ReadingStatsMetricState::NotApplicable, 0}; }
  static constexpr ReadingStatsMetric noData() { return {ReadingStatsMetricState::NoData, 0}; }
};

template <size_t N>
struct ReadingStatsChart {
  std::array<uint32_t, N> seconds{};
  bool available = false;
  // Older stats and intervals recorded before the clock was valid can make
  // the chart cover less time than the cumulative total. The measured bars
  // remain useful, but the UI must disclose that they are incomplete.
  bool incomplete = false;
};

struct BookReadingStatsPresentation {
  ReadingStatsMetric readingTime;
  ReadingStatsMetric sessions;
  ReadingStatsMetric pagesTurned;
  ReadingStatsMetric progress;
  ReadingStatsMetric averageSession;
  ReadingStatsMetric averagePage;
  ReadingStatsMetric timeLeft;
  ReadingStatsMetric completed;
  ReadingStatsMetric startDate;
  ReadingStatsMetric finishDate;
  ReadingStatsChart<READING_TIME_BUCKET_COUNT> timeOfDay;
  ReadingStatsChart<READING_DAY_OF_WEEK_COUNT> dayOfWeek;
};

struct GlobalReadingStatsPresentation {
  ReadingStatsMetric readingTime;
  ReadingStatsMetric sessions;
  ReadingStatsMetric pagesTurned;
  ReadingStatsMetric averageSession;
  ReadingStatsMetric completedBooks;
  ReadingStatsMetric currentStreak;
  ReadingStatsMetric longestStreak;
  ReadingStatsMetric readingDays;
  ReadingStatsChart<READING_TIME_BUCKET_COUNT> timeOfDay;
  ReadingStatsChart<READING_DAY_OF_WEEK_COUNT> dayOfWeek;
};

struct ReadingStatsPresentation {
  BookReadingStatsPresentation book;
  GlobalReadingStatsPresentation device;
  ReadingCalendarSnapshot deviceCalendar;
  GlobalReadingStatsPresentation allSynced;
  uint16_t validPeerCount = 0;
  uint16_t skippedPeerCount = 0;
  bool syncAggregateComplete = false;
  bool showAllSynced = false;
};

ReadingStatsPresentation buildReadingStatsPresentation(const BookReadingStats& bookStats, bool bookStatsTrusted,
                                                       const GlobalReadingStats& deviceStats, bool deviceStatsTrusted,
                                                       const GlobalReadingStatsAggregation& allSyncedStats,
                                                       const ReadingStatsDateTime* now, ReadingStatsMetric progress,
                                                       bool hasFreshTimeEstimate);

// Plain text reflows when typography changes, so page pace and its derived
// finish estimates have no stable meaning. A recorded completion date remains
// meaningful and is preserved.
void markReadingStatsPageMetricsNotApplicable(ReadingStatsPresentation& presentation);

// Maps a chart value to pixels without overflow. A positive value remains
// visible even when it is much smaller than the largest bar.
int scaleReadingStatsBar(uint32_t value, uint32_t maximum, int availableWidth);

// Applies CrossInk v1.4.0's 10-second duration and 60-second session filters,
// but only to caller-owned display copies. A null target represents statistics
// that are currently not writable.
void previewReadingStatsSession(BookReadingStats* bookStats, GlobalReadingStats* deviceStats, uint32_t seconds,
                                const BookReadingStats& pendingBookSpans, const GlobalReadingStats& pendingGlobalSpans,
                                const ReadingStatsDateTime* sessionStart = nullptr);
