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
  agg.recordPageTurn(3000, true);   // +2000 ms, +1 page
  agg.recordPageTurn(8000, true);   // +5000 ms, +1 page
  agg.endSession(9000);             // +1000 ms

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 2u);
  EXPECT_EQ(s->totalReadingMs, 8000u);  // 2000 + 5000 + 1000
  EXPECT_EQ(s->sessionCount, 1u);
}

TEST(ReadingStats, BackwardTurnBanksTimeButNotPages) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);    // +1000 ms, +1 page
  agg.recordPageTurn(2000, false);   // +1000 ms, no page
  agg.endSession(2000);              // +0 ms

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
