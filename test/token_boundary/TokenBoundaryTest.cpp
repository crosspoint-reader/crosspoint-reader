#include <gtest/gtest.h>

#include <iomanip>

#include "lib/Epub/Epub/TokenBoundary.h"

TEST(TokenBoundary, OrdinaryWordGapIsBreakableAndJustifiable) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/false, /*noSpaceBefore=*/false));
  EXPECT_TRUE(TokenBoundary::isJustifiableGap(/*continues=*/false, /*noSpaceBefore=*/false,
                                              /*isSpaceToken=*/false));
}

TEST(TokenBoundary, CjkZeroWidthGapIsBreakableAndJustifiable) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/false, /*noSpaceBefore=*/true));
  EXPECT_TRUE(TokenBoundary::isJustifiableGap(/*continues=*/false, /*noSpaceBefore=*/true,
                                              /*isSpaceToken=*/false));
}

TEST(TokenBoundary, AttachedTextIsNeitherBreakableNorJustifiable) {
  EXPECT_FALSE(TokenBoundary::allowsBreak(/*continues=*/true, /*noSpaceBefore=*/false));
  EXPECT_FALSE(TokenBoundary::isJustifiableGap(/*continues=*/true, /*noSpaceBefore=*/false,
                                               /*isSpaceToken=*/false));
}

TEST(TokenBoundary, ExplicitHyphenBoundaryIsBreakableButNotJustifiable) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/true, /*noSpaceBefore=*/true));
  EXPECT_FALSE(TokenBoundary::isJustifiableGap(/*continues=*/true, /*noSpaceBefore=*/true,
                                               /*isSpaceToken=*/false));
}

TEST(TokenBoundary, LiteralAttachedSpaceRemainsJustifiable) {
  EXPECT_TRUE(TokenBoundary::isJustifiableGap(/*continues=*/true, /*noSpaceBefore=*/false,
                                              /*isSpaceToken=*/true));
}

TEST(TokenBoundary, VisibleExplicitHyphensAllowBreaks) {
  constexpr uint32_t breakableCodepoints[] = {
      '-',    0x058A, 0x2010, 0x2012, 0x2013, 0x2014, 0x2015, 0x2043, 0x207B, 0x208B,
      0x2212, 0x2E17, 0x2E3A, 0x2E3B, 0xFE58, 0xFE63, 0xFF0D, 0x005F, 0x2026,
  };
  for (const uint32_t cp : breakableCodepoints) {
    EXPECT_TRUE(TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) << "codepoint U+" << std::hex << cp;
  }
}

TEST(TokenBoundary, ConditionalAndNonBreakingHyphensKeepTheirSemantics) {
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x00AD));  // soft hyphen
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x2011));  // non-breaking hyphen
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen('.'));
}
