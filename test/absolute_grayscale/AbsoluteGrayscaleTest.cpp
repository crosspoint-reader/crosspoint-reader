#include <gtest/gtest.h>

#include "lib/GfxRenderer/BitmapHelpers.h"

TEST(AbsoluteGrayscale, FullPlanesIncludeBlackWhiteAndBothGrayLevels) {
  uint8_t planes[2] = {0xff, 0xff};
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned x = 0; x < 8; ++x) {
      const auto pixel = grayPlanePixel(x % 4, p == 1, true);
      ASSERT_TRUE(pixel.write);
      if (pixel.black)
        planes[p] &= ~(0x80 >> x);
      else
        planes[p] |= 0x80 >> x;
    }
  }
  EXPECT_EQ(planes[0], 0x55);  // black/dark/light/white = 0/1/0/1
  EXPECT_EQ(planes[1], 0x33);  // black/dark/light/white = 0/0/1/1
}

TEST(AbsoluteGrayscale, OverlayStillLeavesBlackAndWhiteToTheBase) {
  uint8_t planes[2] = {0, 0};
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned x = 0; x < 8; ++x) {
      const auto pixel = grayPlanePixel(x % 4, p == 1, false);
      if (x % 4 == 0 || x % 4 == 3) EXPECT_FALSE(pixel.write);
      if (pixel.write && !pixel.black) planes[p] |= 0x80 >> x;
    }
  }
  EXPECT_EQ(planes[0], 0x44);
  EXPECT_EQ(planes[1], 0x66);
}

TEST(AbsoluteGrayscale, ImageQuantizersRetainFourEvenLevels) {
  for (uint8_t level = 0; level < 4; ++level) {
    AtkinsonDitherer atkinson(1, true);
    FloydSteinbergDitherer floyd(1, true);
    EXPECT_EQ(atkinson.processPixel(level * 85, 0), level);
    EXPECT_EQ(floyd.processPixel(level * 85, 0), level);
  }
  AtkinsonDitherer overlay(1);
  EXPECT_EQ(overlay.processPixel(85, 0), 2);
}

TEST(AbsoluteGrayscale, TransparentPassesRetainBackgroundWithoutClearingBetweenPlanes) {
  // Alternating B/W background, with four opaque pixels and four transparent ones.
  uint8_t frame = 0xaa;
  uint8_t planes[2] = {};
  for (unsigned plane = 0; plane < 2; ++plane) {
    for (unsigned x = 0; x < 4; ++x) {
      const auto pixel = grayPlanePixel(x, plane == 1, true);
      if (pixel.black)
        frame &= ~(0x80 >> x);
      else
        frame |= 0x80 >> x;
    }
    planes[plane] = frame;
  }
  EXPECT_EQ(planes[0], 0x5a);
  EXPECT_EQ(planes[1], 0x3a);
  EXPECT_EQ(planes[0] & 0x0f, 0x0a);
  EXPECT_EQ(planes[1] & 0x0f, 0x0a);
}
