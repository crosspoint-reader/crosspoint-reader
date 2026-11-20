#include <gtest/gtest.h>

#include "BookEnd.h"

namespace {

// A three-chapter book whose last chapter has 4 pages (0..3).
constexpr int SPINES = 3;
constexpr int LAST = SPINES - 1;

TEST(BookEnd, LastPageOfLastChapterIsTheEnd) { EXPECT_TRUE(isLastPageOfBook(LAST, SPINES, 3, 4, true)); }

TEST(BookEnd, EarlierPagesAreNot) {
  EXPECT_FALSE(isLastPageOfBook(LAST, SPINES, 2, 4, true));
  EXPECT_FALSE(isLastPageOfBook(LAST, SPINES, 0, 4, true));
}

TEST(BookEnd, LastPageOfAnEarlierChapterIsNot) { EXPECT_FALSE(isLastPageOfBook(LAST - 1, SPINES, 3, 4, true)); }

TEST(BookEnd, PartialLayoutIsNotTrusted) {
  // While a chapter is still being laid out its total is an estimate that can trail the reader.
  EXPECT_FALSE(isLastPageOfBook(LAST, SPINES, 3, 4, false));
}

TEST(BookEnd, OnePageLastChapterIsTheEndOnArrival) { EXPECT_TRUE(isLastPageOfBook(LAST, SPINES, 0, 1, true)); }

TEST(BookEnd, PageRunningPastTheTotalStillCounts) { EXPECT_TRUE(isLastPageOfBook(LAST, SPINES, 5, 4, true)); }

TEST(BookEnd, UnknownTotalsAreNot) {
  EXPECT_FALSE(isLastPageOfBook(LAST, SPINES, 0, 0, true));
  EXPECT_FALSE(isLastPageOfBook(0, 0, 0, 1, true));
  EXPECT_FALSE(isLastPageOfBook(LAST, SPINES, -1, 4, true));
}

TEST(BookEnd, PercentAtPageEndRounds) {
  EXPECT_EQ(bookPercentAtPageEnd(0.0f, false), 0);
  EXPECT_EQ(bookPercentAtPageEnd(0.594f, false), 59);
  EXPECT_EQ(bookPercentAtPageEnd(0.686f, false), 69);
}

TEST(BookEnd, PercentReachesHundredOnlyAtTheEnd) {
  // An unconfirmed last page (layout still running) or a long book's near-end must not read 100%.
  EXPECT_EQ(bookPercentAtPageEnd(1.0f, false), 99);
  EXPECT_EQ(bookPercentAtPageEnd(0.997f, false), 99);
  EXPECT_EQ(bookPercentAtPageEnd(1.0f, true), 100);
}

TEST(BookEnd, PercentClampsOutOfRangeFractions) { EXPECT_EQ(bookPercentAtPageEnd(-0.2f, false), 0); }

}  // namespace
