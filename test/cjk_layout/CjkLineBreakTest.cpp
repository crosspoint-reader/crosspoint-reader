#include <gtest/gtest.h>
#include <cstdint>
#include "CjkLayout.h"

TEST(CjkLeadingPunctuation, RecognizesAllTwelvePunctuationMarks) {
  EXPECT_TRUE(isCJKLeadingPunctuation(0x3002));  // 。
  EXPECT_TRUE(isCJKLeadingPunctuation(0xFF0C));  // ，
  EXPECT_TRUE(isCJKLeadingPunctuation(0xFF01));  // ！
  EXPECT_TRUE(isCJKLeadingPunctuation(0xFF1F));  // ？
  EXPECT_TRUE(isCJKLeadingPunctuation(0xFF1B));  // ；
  EXPECT_TRUE(isCJKLeadingPunctuation(0xFF1A));  // ：
  EXPECT_TRUE(isCJKLeadingPunctuation(0x3001));  // 、
  EXPECT_TRUE(isCJKLeadingPunctuation(0xFF09));  // ）
  EXPECT_TRUE(isCJKLeadingPunctuation(0x301B));  // 】
  EXPECT_TRUE(isCJKLeadingPunctuation(0x300B));  // 》
  EXPECT_TRUE(isCJKLeadingPunctuation(0x201D));  // "
  EXPECT_TRUE(isCJKLeadingPunctuation(0x2019));  // '
}

TEST(CjkLeadingPunctuation, RejectsCjkIdeographs) {
  EXPECT_FALSE(isCJKLeadingPunctuation(0x4E00));  // 一
  EXPECT_FALSE(isCJKLeadingPunctuation(0x6211));  // 我
  EXPECT_FALSE(isCJKLeadingPunctuation(0x9FFF));  // Last BMP CJK
}

TEST(CjkLeadingPunctuation, RejectsLatinAndOther) {
  EXPECT_FALSE(isCJKLeadingPunctuation(0x0041));  // 'A'
  EXPECT_FALSE(isCJKLeadingPunctuation(0x0020));  // ' '
  EXPECT_FALSE(isCJKLeadingPunctuation(0x002C));  // ',' Latin comma
  EXPECT_FALSE(isCJKLeadingPunctuation(0x0000));  // NUL
}
