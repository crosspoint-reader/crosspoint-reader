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
