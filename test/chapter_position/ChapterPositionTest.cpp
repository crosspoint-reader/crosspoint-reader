#include <gtest/gtest.h>

#include "src/activities/reader/ChapterPosition.h"

// The reader menu header and the go-to-percent seed read a ChapterPosition
// while the section may be released (#3434). A position rebuilt from the
// cached 0-based page and total must render exactly like the live one.

TEST(ChapterPositionTest, DisplayPageIsOneBased) {
  EXPECT_EQ((ChapterPosition{0, 48}).displayPage(), 1);
  EXPECT_EQ((ChapterPosition{10, 48}).displayPage(), 11);
  EXPECT_EQ((ChapterPosition{47, 48}).displayPage(), 48);
}

TEST(ChapterPositionTest, CachedPositionKeepsChapterClause) {
  // nextPageNumber / cachedChapterTotalPageCount as saved before a child
  // screen released the section. The header only shows "Chapter: X/Y" when a
  // total is known, so this is the guard that #3434 tripped.
  const ChapterPosition cached{10, 48};
  EXPECT_TRUE(cached.hasTotal());
  EXPECT_EQ(cached.displayPage(), 11);
  EXPECT_EQ(cached.totalPages, 48);
}

TEST(ChapterPositionTest, ChapterFractionUsesZeroBasedIndex) {
  EXPECT_FLOAT_EQ((ChapterPosition{0, 48}).chapterFraction(), 0.0f);
  EXPECT_FLOAT_EQ((ChapterPosition{10, 48}).chapterFraction(), 10.0f / 48.0f);
  EXPECT_FLOAT_EQ((ChapterPosition{47, 48}).chapterFraction(), 47.0f / 48.0f);
}

TEST(ChapterPositionTest, UnknownTotalHasNoFraction) {
  const ChapterPosition unknown{5, 0};
  EXPECT_FALSE(unknown.hasTotal());
  EXPECT_FLOAT_EQ(unknown.chapterFraction(), 0.0f);
  EXPECT_FALSE((ChapterPosition{}).hasTotal());
}

TEST(ChapterPositionTest, EvaluatesAtCompileTime) {
  static_assert(ChapterPosition{10, 48}.displayPage() == 11);
  static_assert(ChapterPosition{10, 48}.hasTotal());
  static_assert(!ChapterPosition{10, 0}.hasTotal());
}
