#include <gtest/gtest.h>

#include <cstdint>

#include "lib/Epub/Epub/css/CssStyle.h"

namespace {

constexpr int32_t kCssScale = 10000;

int16_t expectedTruncatedPixels(const int hundredths, const CssUnit unit, const int emSize, const int containerWidth) {
  const int64_t scaled = static_cast<int64_t>(hundredths) * 100;
  int64_t numerator = scaled;
  int64_t denominator = kCssScale;

  switch (unit) {
    case CssUnit::Em:
    case CssUnit::Rem:
      numerator *= emSize;
      break;
    case CssUnit::Points:
      numerator *= 133;
      denominator *= 100;
      break;
    case CssUnit::Percent:
      numerator *= containerWidth;
      denominator *= 100;
      break;
    case CssUnit::Pixels:
    default:
      break;
  }

  return static_cast<int16_t>(numerator / denominator);
}

int expectedNearestPixels(const int hundredths, const CssUnit unit, const int emSize, const int containerWidth) {
  const int64_t scaled = static_cast<int64_t>(hundredths) * 100;
  int64_t numerator = scaled;
  int64_t denominator = kCssScale;

  switch (unit) {
    case CssUnit::Em:
    case CssUnit::Rem:
      numerator *= emSize;
      break;
    case CssUnit::Points:
      numerator *= 133;
      denominator *= 100;
      break;
    case CssUnit::Percent:
      numerator *= containerWidth;
      denominator *= 100;
      break;
    case CssUnit::Pixels:
    default:
      break;
  }

  return static_cast<int>((numerator + (denominator / 2)) / denominator);
}

}  // namespace

TEST(CssLength, IntegerValuesRoundTripExactly) {
  constexpr CssUnit kUnits[] = {
      CssUnit::Pixels, CssUnit::Em, CssUnit::Rem, CssUnit::Points, CssUnit::Percent,
  };

  for (const CssUnit unit : kUnits) {
    for (int value = -4096; value <= 4096; ++value) {
      const CssLength len(static_cast<float>(value), unit);
      EXPECT_FLOAT_EQ(len.value(), static_cast<float>(value)) << "value=" << value;
      EXPECT_EQ(len.unit(), unit) << "value=" << value;
      EXPECT_EQ(len.rawPacked() & 0x7u, static_cast<uint32_t>(unit)) << "value=" << value;
    }
  }
}

TEST(CssLength, FractionalValuesRemainPixelExactWhenResolved) {
  constexpr CssUnit kUnits[] = {
      CssUnit::Pixels, CssUnit::Em, CssUnit::Rem, CssUnit::Points, CssUnit::Percent,
  };
  constexpr float kContainerWidths[] = {
      1.0f, 37.0f, 64.0f, 123.0f, 240.0f, 255.0f, 320.0f, 480.0f, 511.0f, 528.0f, 600.0f, 758.0f, 800.0f, 997.0f,
  };

  for (const CssUnit unit : kUnits) {
    for (int hundredths = 1; hundredths <= 20000; ++hundredths) {
      const float value = static_cast<float>(hundredths) / 100.0f;
      for (int emSize = 5; emSize <= 32; ++emSize) {
        for (const int containerWidth : kContainerWidths) {
          const CssLength packed(value, unit);
          const int16_t expectedTruncated = expectedTruncatedPixels(hundredths, unit, emSize, containerWidth);
          const int expectedNearest = expectedNearestPixels(hundredths, unit, emSize, containerWidth);

          EXPECT_EQ(packed.toPixelsInt16(static_cast<float>(emSize), static_cast<float>(containerWidth)),
                    expectedTruncated)
              << "unit=" << static_cast<int>(unit) << " value=" << value << " em=" << emSize
              << " container=" << containerWidth;

          EXPECT_EQ(
              static_cast<int>(packed.toPixels(static_cast<float>(emSize), static_cast<float>(containerWidth)) + 0.5f),
              expectedNearest)
              << "unit=" << static_cast<int>(unit) << " value=" << value << " em=" << emSize
              << " container=" << containerWidth;
        }
      }
    }
  }
}
