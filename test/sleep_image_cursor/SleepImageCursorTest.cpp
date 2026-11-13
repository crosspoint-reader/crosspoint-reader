#include <gtest/gtest.h>

#include "SleepImageCursor.h"

TEST(SleepImageCursor, ColdBootUsesCardPosition) {
  SleepImageCursor cursor{};
  EXPECT_STREQ(cursor.lastShown("/.sleep/2.bmp"), "/.sleep/2.bmp");
}

TEST(SleepImageCursor, FailedSavesKeepAdvancingPastTheStaleCardPosition) {
  SleepImageCursor cursor{};
  ASSERT_TRUE(cursor.record("/.sleep/1.bmp", false));
  EXPECT_STREQ(cursor.lastShown(""), "/.sleep/1.bmp");
  ASSERT_TRUE(cursor.record("/.sleep/2.bmp", false));
  EXPECT_STREQ(cursor.lastShown("/.sleep/1.bmp"), "/.sleep/2.bmp");
  ASSERT_TRUE(cursor.record("/.sleep/3.bmp", false));
  EXPECT_STREQ(cursor.lastShown("/.sleep/1.bmp"), "/.sleep/3.bmp");
}

TEST(SleepImageCursor, SuccessfulSaveReturnsAuthorityToTheCard) {
  SleepImageCursor cursor{};
  ASSERT_TRUE(cursor.record("/.sleep/2.bmp", false));
  ASSERT_TRUE(cursor.record("/.sleep/3.bmp", true));
  EXPECT_STREQ(cursor.lastShown("/.sleep/3.bmp"), "/.sleep/3.bmp");
  EXPECT_STREQ(cursor.lastShown("/.sleep/external.bmp"), "/.sleep/external.bmp");
}

TEST(SleepImageCursor, ResetDiscardsAnUnsavedPosition) {
  SleepImageCursor cursor{};
  ASSERT_TRUE(cursor.record("/.sleep/3.bmp", false));
  cursor = SleepImageCursor{};
  EXPECT_STREQ(cursor.lastShown("/.sleep/1.bmp"), "/.sleep/1.bmp");
}

TEST(SleepImageCursor, BoundsAndTerminatesTheRetainedPath) {
  SleepImageCursor cursor{};
  const std::string longest(SleepImageCursor::PATH_CAPACITY - 1, 'x');
  ASSERT_TRUE(cursor.record(longest, false));
  EXPECT_STREQ(cursor.lastShown(""), longest.c_str());
  EXPECT_FALSE(cursor.record(longest + "x", false));
  EXPECT_STREQ(cursor.lastShown("saved"), "saved");
  EXPECT_FALSE(cursor.record("", false));
}
