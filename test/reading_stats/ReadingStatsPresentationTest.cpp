#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

#include "ReadingStatsPresentation.h"

namespace {
ReadingStatsPresentation build(const BookReadingStats& book, const bool bookTrusted, const GlobalReadingStats& device,
                               const bool deviceTrusted, const GlobalReadingStatsAggregation& aggregate,
                               const ReadingStatsDateTime* now = nullptr,
                               const ReadingStatsMetric progress = ReadingStatsMetric::unavailable(),
                               const bool freshEstimate = false) {
  return buildReadingStatsPresentation(book, bookTrusted, device, deviceTrusted, aggregate, now, progress,
                                       freshEstimate);
}
}  // namespace

TEST(ReadingStatsPresentation, TrustedMissingDataKeepsRealZeroDistinctFromUnavailable) {
  const BookReadingStats book;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::estimated(0));

  EXPECT_EQ(model.book.readingTime.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.readingTime.value, 0u);
  EXPECT_EQ(model.book.sessions.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.averageSession.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.averagePage.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.progress.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.book.progress.value, 0u);
  EXPECT_TRUE(model.book.timeOfDay.available);
  EXPECT_FALSE(model.book.timeOfDay.incomplete);

  EXPECT_EQ(model.device.readingTime.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.currentStreak.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.device.longestStreak.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.longestStreak.value, 0u);
  EXPECT_EQ(model.device.readingDays.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.readingDays.value, 0u);
  EXPECT_FALSE(model.showAllSynced);
}

TEST(ReadingStatsPresentation, UntrustedFilesNeverBecomePlausibleZeroes) {
  BookReadingStats book;
  book.totalReadingSeconds = 123;
  GlobalReadingStats device;
  device.totalReadingSeconds = 456;
  GlobalReadingStatsAggregation aggregate{device, 1, 2};

  const ReadingStatsPresentation model =
      build(book, false, device, false, aggregate, nullptr, ReadingStatsMetric::estimated(42), true);
  EXPECT_EQ(model.book.readingTime.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.sessions.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.progress.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.device.readingTime.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.allSynced.readingTime.state, ReadingStatsMetricState::Unavailable);
  EXPECT_TRUE(model.showAllSynced);
  EXPECT_EQ(model.validPeerCount, 1u);
  EXPECT_EQ(model.skippedPeerCount, 2u);
  EXPECT_FALSE(model.syncAggregateComplete);
}

TEST(ReadingStatsPresentation, CombinedTotalsRequireEveryInputAndDirectoryEntryToBeVerified) {
  const BookReadingStats book;
  GlobalReadingStats device;
  device.totalReadingSeconds = 10;
  GlobalReadingStatsAggregation aggregate{device, 1, 0, true};

  ReadingStatsPresentation model = build(book, true, device, true, aggregate);
  EXPECT_TRUE(model.showAllSynced);
  EXPECT_TRUE(model.syncAggregateComplete);
  EXPECT_EQ(model.allSynced.readingTime.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.allSynced.completedBooks.state, ReadingStatsMetricState::Unavailable);

  aggregate.skippedPeerCount = 1;
  model = build(book, true, device, true, aggregate);
  EXPECT_FALSE(model.syncAggregateComplete);
  EXPECT_EQ(model.allSynced.readingTime.state, ReadingStatsMetricState::Unavailable);

  aggregate.skippedPeerCount = 0;
  aggregate.scanComplete = false;
  model = build(book, true, device, true, aggregate);
  EXPECT_FALSE(model.syncAggregateComplete);
  EXPECT_EQ(model.allSynced.readingTime.state, ReadingStatsMetricState::Unavailable);

  aggregate.scanComplete = true;
  model = build(book, true, device, false, aggregate);
  EXPECT_FALSE(model.syncAggregateComplete);
  EXPECT_EQ(model.allSynced.readingTime.state, ReadingStatsMetricState::Unavailable);
}

TEST(ReadingStatsPresentation, SyncedCompletedBooksStayUnavailableWithoutBookIdentity) {
  const BookReadingStats book;
  GlobalReadingStats device;
  device.completedBooks = 1;
  GlobalReadingStatsAggregation aggregate{device, 1, 0, true};
  aggregate.stats.completedBooks = 2;

  const ReadingStatsPresentation model = build(book, true, device, true, aggregate);
  EXPECT_EQ(model.device.completedBooks.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.completedBooks.value, 1u);
  EXPECT_TRUE(model.syncAggregateComplete);
  EXPECT_EQ(model.allSynced.completedBooks.state, ReadingStatsMetricState::Unavailable);
}

TEST(ReadingStatsPresentation, EstimateRequiresFreshStablePagination) {
  BookReadingStats book;
  book.totalReadingSeconds = 600;
  book.sessionCount = 2;
  book.avgSecondsPerForwardPage = 20;
  book.paceSampleCount = 3;
  book.estimatedTimeLeftSeconds = 1200;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};

  ReadingStatsPresentation model = build(book, true, device, true, aggregate);
  EXPECT_EQ(model.book.averageSession.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.averagePage.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::Unavailable);

  model = build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::unavailable(), true);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.book.timeLeft.value, 1200u);

  book.isCompleted = true;
  model = build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::known(100), true);
  EXPECT_EQ(model.book.completed.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.completed.value, 1u);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.timeLeft.value, 0u);
}

TEST(ReadingStatsPresentation, DatedChartsExposeLegacyAndPartialCoverage) {
  BookReadingStats book;
  book.totalReadingSeconds = 100;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};

  ReadingStatsPresentation model = build(book, true, device, true, aggregate);
  EXPECT_FALSE(model.book.timeOfDay.available);
  EXPECT_FALSE(model.book.dayOfWeek.available);

  book.timeOfDaySeconds[0] = 30;
  book.dayOfWeekSeconds[1] = 30;
  model = build(book, true, device, true, aggregate);
  EXPECT_TRUE(model.book.timeOfDay.available);
  EXPECT_TRUE(model.book.timeOfDay.incomplete);
  EXPECT_TRUE(model.book.dayOfWeek.available);
  EXPECT_TRUE(model.book.dayOfWeek.incomplete);

  book.timeOfDaySeconds[1] = 70;
  book.dayOfWeekSeconds[2] = 70;
  model = build(book, true, device, true, aggregate);
  EXPECT_FALSE(model.book.timeOfDay.incomplete);
  EXPECT_FALSE(model.book.dayOfWeek.incomplete);
}

TEST(ReadingStatsPresentation, BookDatesDistinguishStoredAndEstimatedValues) {
  BookReadingStats book;
  book.totalReadingSeconds = 3600;
  book.startDate = {2024, 1, 1};
  book.estimatedTimeLeftSeconds = 7200;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsDateTime now{{2024, 1, 11}, 12, 0, 0};

  ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, &now, ReadingStatsMetric::estimated(50), true);
  EXPECT_EQ(model.book.startDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.startDate.value, readingStatsDayIndex(book.startDate));
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.book.finishDate.value, readingStatsDayIndex({2024, 1, 31}));

  book.isCompleted = true;
  book.finishedDate = {2024, 1, 10};
  model = build(book, true, device, true, aggregate, &now, ReadingStatsMetric::known(100), true);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.finishDate.value, readingStatsDayIndex(book.finishedDate));
}

TEST(ReadingStatsPresentation, FinishEstimateRequiresAValidNonFutureClockAndFitsCalendar) {
  BookReadingStats book;
  book.totalReadingSeconds = 1;
  book.startDate = {2024, 1, 12};
  book.estimatedTimeLeftSeconds = std::numeric_limits<uint32_t>::max();
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsDateTime now{{2024, 1, 11}, 12, 0, 0};

  ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, &now, ReadingStatsMetric::estimated(1), true);
  EXPECT_EQ(model.book.startDate.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Unavailable);

  const ReadingStatsDateTime invalidNow{};
  model = build(book, true, device, true, aggregate, &invalidNow, ReadingStatsMetric::estimated(1), true);
  EXPECT_EQ(model.book.startDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Unavailable);

  book.startDate = {2000, 1, 1};
  const ReadingStatsDateTime endOfCalendar{{2099, 12, 31}, 23, 59, 59};
  model = build(book, true, device, true, aggregate, &endOfCalendar, ReadingStatsMetric::estimated(99), true);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Unavailable);
}

TEST(ReadingStatsPresentation, FutureCompletionDateIsNotPresentedAsFact) {
  BookReadingStats book;
  book.isCompleted = true;
  book.startDate = {2024, 1, 1};
  book.finishedDate = {2024, 1, 12};
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsDateTime now{{2024, 1, 11}, 12, 0, 0};

  const ReadingStatsPresentation model = build(book, true, device, true, aggregate, &now);
  EXPECT_EQ(model.book.startDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Unavailable);
}

TEST(ReadingStatsPresentation, GlobalHistoryReportsExactRollingDaysAndLongestStreak) {
  GlobalReadingStats device;
  device.totalReadingSeconds = 180;
  device.recordReadingSpan({{2024, 1, 1}, 12, 0, 0}, 60);
  device.recordReadingSpan({{2024, 1, 2}, 12, 0, 0}, 60);
  device.recordReadingSpan({{2024, 1, 4}, 12, 0, 0}, 60);
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const BookReadingStats book;

  ReadingStatsPresentation model = build(book, true, device, true, aggregate);
  EXPECT_EQ(model.device.readingDays.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.readingDays.value, 3u);
  EXPECT_EQ(model.device.longestStreak.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.longestStreak.value, 2u);

  device.readingHistoryAnchorDay = 0;
  device.readingHistoryBits.fill(0);
  device.longestReadingStreak = 0;
  model = build(book, true, device, true, {device, 0, 0});
  EXPECT_EQ(model.device.readingDays.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.device.longestStreak.state, ReadingStatsMetricState::Unavailable);
}

TEST(ReadingStatsPresentation, StreakNeedsAValidCurrentDateButKnownZeroStaysZero) {
  GlobalReadingStats device;
  device.recordReadingSpan({{2024, 1, 2}, 12, 0, 0}, 60);
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const BookReadingStats book;

  ReadingStatsPresentation model = build(book, true, device, true, aggregate);
  EXPECT_EQ(model.device.currentStreak.state, ReadingStatsMetricState::Unavailable);

  const ReadingStatsDateTime sameDay{{2024, 1, 2}, 12, 0, 0};
  model = build(book, true, device, true, aggregate, &sameDay);
  EXPECT_EQ(model.device.currentStreak.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.currentStreak.value, 1u);

  const ReadingStatsDateTime muchLater{{2024, 1, 10}, 12, 0, 0};
  model = build(book, true, device, true, aggregate, &muchLater);
  EXPECT_EQ(model.device.currentStreak.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.device.currentStreak.value, 0u);
}

TEST(ReadingStatsPresentation, BarScalingIsBoundedVisibleAndOverflowSafe) {
  EXPECT_EQ(scaleReadingStatsBar(0, 100, 50), 0);
  EXPECT_EQ(scaleReadingStatsBar(1, 1000, 50), 2);
  EXPECT_EQ(scaleReadingStatsBar(1000, 1000, 50), 50);
  EXPECT_EQ(scaleReadingStatsBar(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), 73), 73);
  EXPECT_EQ(scaleReadingStatsBar(10, 0, 50), 0);
  EXPECT_EQ(scaleReadingStatsBar(10, 20, 0), 0);
  EXPECT_EQ(scaleReadingStatsBar(1, 1000, 1), 1);
}

TEST(ReadingStatsPresentation, SessionPreviewMatchesCommitNoiseThresholdsWithoutMutatingSources) {
  BookReadingStats sourceBook;
  sourceBook.totalReadingSeconds = 100;
  sourceBook.sessionCount = 3;
  GlobalReadingStats sourceDevice;
  sourceDevice.totalReadingSeconds = 200;
  sourceDevice.totalSessions = 4;
  BookReadingStats pendingBook;
  pendingBook.timeOfDaySeconds[0] = 9;
  pendingBook.dayOfWeekSeconds[1] = 9;
  GlobalReadingStats pendingDevice;
  pendingDevice.timeOfDaySeconds[0] = 9;
  pendingDevice.dayOfWeekSeconds[1] = 9;

  for (const uint32_t seconds : {9u, 10u, 59u, 60u}) {
    BookReadingStats book = sourceBook;
    GlobalReadingStats device = sourceDevice;
    previewReadingStatsSession(&book, &device, seconds, pendingBook, pendingDevice);

    const bool keepsDuration = seconds >= 10;
    const bool countsSession = seconds >= 60;
    EXPECT_EQ(book.totalReadingSeconds, 100u + (keepsDuration ? seconds : 0u));
    EXPECT_EQ(device.totalReadingSeconds, 200u + (keepsDuration ? seconds : 0u));
    EXPECT_EQ(book.timeOfDaySeconds[0], keepsDuration ? 9u : 0u);
    EXPECT_EQ(device.dayOfWeekSeconds[1], keepsDuration ? 9u : 0u);
    EXPECT_EQ(book.sessionCount, 3u + (countsSession ? 1u : 0u));
    EXPECT_EQ(device.totalSessions, 4u + (countsSession ? 1u : 0u));
  }

  EXPECT_EQ(sourceBook.totalReadingSeconds, 100u);
  EXPECT_EQ(sourceDevice.totalReadingSeconds, 200u);
}

TEST(ReadingStatsPresentation, SessionPreviewCanLeaveReadOnlyScopesUntouched) {
  BookReadingStats book;
  GlobalReadingStats device;
  const BookReadingStats pendingBook;
  const GlobalReadingStats pendingDevice;
  previewReadingStatsSession(nullptr, &device, 60, pendingBook, pendingDevice);
  EXPECT_EQ(book.totalReadingSeconds, 0u);
  EXPECT_EQ(book.sessionCount, 0u);
  EXPECT_EQ(device.totalReadingSeconds, 60u);
  EXPECT_EQ(device.totalSessions, 1u);
}

TEST(ReadingStatsPresentation, MixedShortVisitsCannotProduceAFakeAverageSession) {
  BookReadingStats book;
  GlobalReadingStats device;
  const BookReadingStats pendingBook;
  const GlobalReadingStats pendingDevice;
  previewReadingStatsSession(&book, &device, 30, pendingBook, pendingDevice);
  previewReadingStatsSession(&book, &device, 60, pendingBook, pendingDevice);
  ASSERT_EQ(book.totalReadingSeconds, 90u);
  ASSERT_EQ(book.sessionCount, 1u);
  ASSERT_EQ(device.totalReadingSeconds, 90u);
  ASSERT_EQ(device.totalSessions, 1u);

  const ReadingStatsPresentation model = build(book, true, device, true, {device, 0, 0});
  EXPECT_EQ(model.book.averageSession.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.device.averageSession.state, ReadingStatsMetricState::Unavailable);
}
