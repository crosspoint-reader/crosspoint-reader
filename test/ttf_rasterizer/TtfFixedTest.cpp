// FreeType's fixed-point helpers, to the bit. The expected scales are what FreeType 2.13 itself reports for
// the bundled fonts at 150 dpi (14 pt Bookerly: x_scale 122356; 14 pt Inter: 59744).
#include <gtest/gtest.h>

#include <cstdint>

#include "TtfFixed.h"

using namespace ttf;

TEST(TtfFixed, MulFixRoundsHalfAwayFromZero) {
  EXPECT_EQ(5, mulFix(5, 0x10000));
  EXPECT_EQ(1, mulFix(1, 0x8000));    // 0.5
  EXPECT_EQ(-1, mulFix(-1, 0x8000));  // -0.5
  EXPECT_EQ(0, mulFix(1, 0x7FFF));
  EXPECT_EQ(-2, mulFix(3, -0x8000));  // -1.5
  EXPECT_EQ(1973, mulFix(1057, 122356));
  EXPECT_EQ(-487, mulFix(-261, 122356));
}

TEST(TtfFixed, MulFixWideKeepsTheHighBits) {
  EXPECT_EQ(int64_t{1} << 34, mulFixWide(1 << 24, 1 << 26));
  EXPECT_EQ(-(int64_t{1} << 34), mulFixWide(-(1 << 24), 1 << 26));
}

TEST(TtfFixed, DivFixGivesFreeTypesScales) {
  EXPECT_EQ(122356, divFix(1867, 1000));  // 14 pt, 1000 units per em
  EXPECT_EQ(104858, divFix(1600, 1000));  // 12 pt
  EXPECT_EQ(59744, divFix(1867, 2048));   // 14 pt, 2048 units per em
  EXPECT_EQ(76800, divFix(2400, 2048));   // 18 pt
  EXPECT_EQ(-122356, divFix(-1867, 1000));
  EXPECT_EQ(0x7FFFFFFF, divFix(1, 0));
}

TEST(TtfFixed, MulDivRoundsHalfAwayFromZero) {
  EXPECT_EQ(955906, mulDiv(500, 122356, 64));  // a 500-unit advance at 14 pt, in 16.16
  EXPECT_EQ(-955906, mulDiv(-500, 122356, 64));
  EXPECT_EQ(2, mulDiv(3, 1, 2));  // 1.5
  EXPECT_EQ(0x7FFFFFFF, mulDiv(1, 1, 0));
}
