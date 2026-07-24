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

TEST(ReadingStatsPresentation, TrustedMissingDataKeepsRealZeroDistinctFromMissingAndUnavailable) {
  const BookReadingStats book;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::estimated(0));

  EXPECT_EQ(model.book.readingTime.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.readingTime.value, 0u);
  EXPECT_EQ(model.book.sessions.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.averageSession.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.averagePage.state, ReadingStatsMetricState::NoData);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::NoData);
  EXPECT_EQ(model.book.startDate.state, ReadingStatsMetricState::NoData);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::NoData);
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
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::NoData);

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

TEST(ReadingStatsPresentation, TextReaderCanReportProgressWithoutInventingLayoutDependentPace) {
  BookReadingStats book;
  book.totalReadingSeconds = 120;
  book.totalPagesTurned = 4;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};

  ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::estimated(37), false);
  markReadingStatsPageMetricsNotApplicable(model);
  EXPECT_EQ(model.book.readingTime.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.readingTime.value, 120u);
  EXPECT_EQ(model.book.pagesTurned.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.pagesTurned.value, 4u);
  EXPECT_EQ(model.book.progress.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.book.progress.value, 37u);
  EXPECT_EQ(model.book.averagePage.state, ReadingStatsMetricState::NotApplicable);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::NotApplicable);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::NotApplicable);
}

TEST(ReadingStatsPresentation, TextReaderKeepsRecordedCompletionFacts) {
  BookReadingStats book;
  book.isCompleted = true;
  book.finishedDate = {2024, 4, 5};
  book.finishedMinuteOfDay = 9u * 60u + 15u;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsDateTime now{{2024, 4, 6}, 12, 0, 0};

  ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, &now, ReadingStatsMetric::estimated(100), false);
  markReadingStatsPageMetricsNotApplicable(model);

  EXPECT_EQ(model.book.averagePage.state, ReadingStatsMetricState::NotApplicable);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.timeLeft.value, 0u);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.finishDate.value, readingStatsMinuteIndex(book.finishedDate, book.finishedMinuteOfDay));
}

TEST(ReadingStatsPresentation, EstimatedProgressCannotClaimCompletion) {
  BookReadingStats book;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};

  ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::estimated(100));
  EXPECT_EQ(model.book.progress.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.book.progress.value, 99u);

  book.isCompleted = true;
  model = build(book, true, device, true, aggregate, nullptr, ReadingStatsMetric::estimated(99));
  EXPECT_EQ(model.book.progress.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.progress.value, 100u);
}

TEST(ReadingStatsPresentation, TextReaderDoesNotHideUntrustedCompletionDataAsNotApplicable) {
  const BookReadingStats book;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};

  ReadingStatsPresentation model = build(book, false, device, true, aggregate);
  markReadingStatsPageMetricsNotApplicable(model);

  EXPECT_EQ(model.book.averagePage.state, ReadingStatsMetricState::NotApplicable);
  EXPECT_EQ(model.book.completed.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.timeLeft.state, ReadingStatsMetricState::Unavailable);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Unavailable);
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
  book.startMinuteOfDay = 8u * 60u + 30u;
  book.estimatedTimeLeftSeconds = 7200;
  const GlobalReadingStats device;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const ReadingStatsDateTime now{{2024, 1, 11}, 12, 0, 0};

  ReadingStatsPresentation model =
      build(book, true, device, true, aggregate, &now, ReadingStatsMetric::estimated(50), true);
  EXPECT_EQ(model.book.startDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.startDate.value, readingStatsMinuteIndex(book.startDate, book.startMinuteOfDay));
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Estimated);
  EXPECT_EQ(model.book.finishDate.value, readingStatsMinuteIndex({2024, 1, 31}, 12u * 60u));

  book.isCompleted = true;
  book.finishedDate = {2024, 1, 10};
  book.finishedMinuteOfDay = 21u * 60u + 5u;
  model = build(book, true, device, true, aggregate, &now, ReadingStatsMetric::known(100), true);
  EXPECT_EQ(model.book.finishDate.state, ReadingStatsMetricState::Known);
  EXPECT_EQ(model.book.finishDate.value, readingStatsMinuteIndex(book.finishedDate, book.finishedMinuteOfDay));
}

TEST(ReadingStatsPresentation, FinishEstimateRequiresAValidNonFutureClockAndFitsCalendar) {
  BookReadingStats book;
  book.totalReadingSeconds = 1;
  book.startDate = {2024, 1, 12};
  book.startMinuteOfDay = 7u * 60u;
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
  book.startMinuteOfDay = 7u * 60u;
  book.finishedDate = {2024, 1, 12};
  book.finishedMinuteOfDay = 8u * 60u;
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

TEST(ReadingStatsPresentation, CalendarSnapshotUsesOnlyTrustedLocalDatedHistory) {
  GlobalReadingStats device;
  device.recordReadingSpan({{2026, 7, 23}, 12, 0, 0}, 60);
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const BookReadingStats book;
  const ReadingStatsDateTime now{{2026, 7, 24}, 8, 0, 0};

  ReadingStatsPresentation model = build(book, true, device, true, aggregate, &now);
  EXPECT_TRUE(model.deviceCalendar.clockValid);
  EXPECT_TRUE(model.deviceCalendar.historyAvailable);
  EXPECT_EQ(model.deviceCalendar.today.day, 24u);
  EXPECT_EQ(model.deviceCalendar.readingDays, 1u);
  EXPECT_EQ(model.deviceCalendar.currentStreak, 1u);

  model = build(book, true, device, false, aggregate, &now);
  EXPECT_FALSE(model.deviceCalendar.historyAvailable);
  EXPECT_FALSE(model.deviceCalendar.clockValid);
}

TEST(ReadingStatsPresentation, CalendarDoesNotPresentLegacyUndatedTotalsAsEmptyDays) {
  GlobalReadingStats device;
  device.totalReadingSeconds = 3600;
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const BookReadingStats book;
  const ReadingStatsDateTime now{{2026, 7, 24}, 8, 0, 0};

  const ReadingStatsPresentation model = build(book, true, device, true, aggregate, &now);
  EXPECT_TRUE(model.deviceCalendar.clockValid);
  EXPECT_FALSE(model.deviceCalendar.historyAvailable);
}

TEST(ReadingStatsPresentation, CalendarRejectsHistoryAnchoredInTheFuture) {
  GlobalReadingStats device;
  device.recordReadingSpan({{2026, 7, 25}, 12, 0, 0}, 60);
  const GlobalReadingStatsAggregation aggregate{device, 0, 0};
  const BookReadingStats book;
  const ReadingStatsDateTime now{{2026, 7, 24}, 8, 0, 0};

  const ReadingStatsPresentation model = build(book, true, device, true, aggregate, &now);
  EXPECT_TRUE(model.deviceCalendar.clockValid);
  EXPECT_FALSE(model.deviceCalendar.historyAvailable);
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

TEST(ReadingStatsPresentation, SessionPreviewIncludesAnEarnedStartDate) {
  BookReadingStats book;
  GlobalReadingStats device;
  const BookReadingStats pendingBook;
  const GlobalReadingStats pendingDevice;
  const ReadingStatsDateTime sessionStart{{2026, 7, 21}, 14, 0, 0};

  previewReadingStatsSession(&book, &device, 119, pendingBook, pendingDevice, &sessionStart);
  EXPECT_FALSE(book.startDate.isValid());

  book = {};
  device = {};
  previewReadingStatsSession(&book, &device, 120, pendingBook, pendingDevice, &sessionStart);
  EXPECT_EQ(book.startDate.year, sessionStart.date.year);
  EXPECT_EQ(book.startDate.month, sessionStart.date.month);
  EXPECT_EQ(book.startDate.day, sessionStart.date.day);
  EXPECT_EQ(book.startMinuteOfDay, 14u * 60u);
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
