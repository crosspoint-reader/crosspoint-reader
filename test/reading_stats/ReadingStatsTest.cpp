#include <gtest/gtest.h>

#include "ReadingStats.h"

using reading_stats::BookStats;
using reading_stats::ReadingStatsAggregator;

TEST(ReadingStats, EmptyAggregatorHasNoData) {
  ReadingStatsAggregator agg;
  EXPECT_EQ(agg.totalPagesRead(), 0u);
  EXPECT_EQ(agg.totalReadingMs(), 0u);
  EXPECT_EQ(agg.statsFor("/books/missing.epub"), nullptr);
  EXPECT_EQ(agg.pagesPerHour("/books/missing.epub"), 0u);
  EXPECT_TRUE(agg.books().empty());
}

TEST(ReadingStats, BanksTimeAndCountsForwardPages) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 1000);
  agg.recordPageTurn(3000, true);  // +2000 ms, +1 page
  agg.recordPageTurn(8000, true);  // +5000 ms, +1 page
  agg.endSession(9000);            // +1000 ms

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 2u);
  EXPECT_EQ(s->totalReadingMs, 8000u);  // 2000 + 5000 + 1000
  EXPECT_EQ(s->sessionCount, 1u);
}

TEST(ReadingStats, BackwardTurnBanksTimeButNotPages) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);   // +1000 ms, +1 page
  agg.recordPageTurn(2000, false);  // +1000 ms, no page
  agg.endSession(2000);             // +0 ms

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 1u);
  EXPECT_EQ(s->totalReadingMs, 2000u);
}

TEST(ReadingStats, PageTurnWithoutSessionIsIgnored) {
  ReadingStatsAggregator agg;
  agg.recordPageTurn(1000, true);
  agg.endSession(2000);
  EXPECT_EQ(agg.totalPagesRead(), 0u);
  EXPECT_EQ(agg.totalReadingMs(), 0u);
}

TEST(ReadingStats, LongGapIsCappedAtMaxPageMs) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  // Gap of 1 hour on one page: only kMaxPageMs (5 min) should be counted.
  agg.recordPageTurn(3600000, true);
  agg.endSession(3600000);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->totalReadingMs, ReadingStatsAggregator::kMaxPageMs);
}

TEST(ReadingStats, MillisWrapCountsAsZeroDelta) {
  ReadingStatsAggregator agg;
  // Start near the uint32 millis() ceiling, then wrap past zero.
  agg.beginSession("/books/a.epub", 0xFFFFFF00u);
  agg.recordPageTurn(0x00000100u, true);  // nowMs < lastEventMs_ -> 0 ms
  agg.endSession(0x00000100u);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 1u);
  EXPECT_EQ(s->totalReadingMs, 0u);
}

TEST(ReadingStats, TracksTwoBooksIndependently) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);
  agg.endSession(1000);

  agg.beginSession("/books/b.epub", 5000);
  agg.recordPageTurn(7000, true);
  agg.recordPageTurn(9000, true);
  agg.endSession(9000);

  EXPECT_EQ(agg.statsFor("/books/a.epub")->pagesRead, 1u);
  EXPECT_EQ(agg.statsFor("/books/b.epub")->pagesRead, 2u);
  EXPECT_EQ(agg.totalPagesRead(), 3u);
}

TEST(ReadingStats, BeginSessionEndsThePriorSession) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);  // book a: +1000 ms, +1 page
  // Switch to b without an explicit endSession; a is banked at nowMs (2000).
  agg.beginSession("/books/b.epub", 2000);
  agg.recordPageTurn(3000, true);  // book b: +1000 ms, +1 page
  agg.endSession(3000);

  const BookStats* a = agg.statsFor("/books/a.epub");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->totalReadingMs, 2000u);  // 1000 (to first turn) + 1000 (banked on switch)
  EXPECT_EQ(a->sessionCount, 1u);       // switching ended a's session

  const BookStats* b = agg.statsFor("/books/b.epub");
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->totalReadingMs, 1000u);
  EXPECT_EQ(b->sessionCount, 1u);
}

TEST(ReadingStats, ReopeningSameBookAccumulates) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);
  agg.endSession(1000);

  agg.beginSession("/books/a.epub", 2000);
  agg.recordPageTurn(3000, true);
  agg.endSession(3000);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 2u);
  EXPECT_EQ(s->totalReadingMs, 2000u);
  EXPECT_EQ(s->sessionCount, 2u);
  EXPECT_EQ(agg.books().size(), 1u);  // not duplicated
}

TEST(ReadingStats, PagesPerHourComputesSpeed) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  // 30 pages over exactly 1 hour -> 30 pages/hour. Use forward turns and a
  // final endSession that adds no extra time (each gap <= kMaxPageMs).
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) {
    t += 120000;  // 2 min per page (under the 5 min cap)
    agg.recordPageTurn(t, true);
  }
  agg.endSession(t);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 30u);
  EXPECT_EQ(s->totalReadingMs, 3600000u);  // 30 * 120000
  EXPECT_EQ(agg.pagesPerHour("/books/a.epub"), 30u);
}

TEST(ReadingStats, PagesPerHourIsZeroWithoutTime) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 1000);
  agg.recordPageTurn(1000, true);  // zero elapsed
  agg.endSession(1000);
  EXPECT_EQ(agg.pagesPerHour("/books/a.epub"), 0u);
}

TEST(ReadingStats, LoadReplacesContentsAndResetsSession) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/stale.epub", 0);  // active session that load() must drop

  std::vector<BookStats> persisted;
  persisted.push_back(BookStats{"/books/a.epub", 10, 600000, 3});
  persisted.push_back(BookStats{"/books/b.epub", 5, 300000, 1});
  agg.load(persisted);

  EXPECT_EQ(agg.books().size(), 2u);
  EXPECT_EQ(agg.totalPagesRead(), 15u);
  EXPECT_EQ(agg.statsFor("/books/a.epub")->sessionCount, 3u);
  EXPECT_EQ(agg.statsFor("/books/stale.epub"), nullptr);

  // After load the prior session is gone, so a stray page turn is ignored
  // until a new beginSession.
  agg.recordPageTurn(1000, true);
  EXPECT_EQ(agg.totalPagesRead(), 15u);
}
