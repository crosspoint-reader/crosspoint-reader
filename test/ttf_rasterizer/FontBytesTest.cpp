// FontBytes: big-endian reads, every one bounds-checked against the file.
#include <gtest/gtest.h>

#include <cstdint>

#include "FontBytes.h"

namespace {
const uint8_t kData[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
}

TEST(FontBytes, ReadsBigEndian) {
  FontBytes bytes(kData, sizeof kData);
  EXPECT_EQ(0x12, bytes.u8(0));
  EXPECT_EQ(0x3456, bytes.u16(1));
  EXPECT_EQ(-0x6544, bytes.i16(4));  // 0x9ABC
  EXPECT_EQ(0x3456789Au, bytes.u32(1));
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_FALSE(bytes.ioError());
}

TEST(FontBytes, TheLastBytesAreInRangeAndOneMoreIsNot) {
  FontBytes bytes(kData, sizeof kData);
  EXPECT_EQ(0xBC, bytes.u8(5));
  EXPECT_EQ(0x9ABC, bytes.u16(4));
  EXPECT_EQ(0x56789ABCu, bytes.u32(2));
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_EQ(0u, bytes.u32(3));
  EXPECT_TRUE(bytes.outOfRange());
}

TEST(FontBytes, EveryKindOfReadPastTheEndReturnsZeroAndSetsTheFlag) {
  for (int kind = 0; kind < 4; ++kind) {
    FontBytes bytes(kData, sizeof kData);
    switch (kind) {
      case 0:
        EXPECT_EQ(0, bytes.u8(6));
        break;
      case 1:
        EXPECT_EQ(0, bytes.u16(5));
        break;
      case 2:
        EXPECT_EQ(0, bytes.i16(0xFFFFFFFFu));  // the offset wraps: still refused
        break;
      default:
        EXPECT_EQ(0u, bytes.u32(0xFFFFFFFEu));
    }
    EXPECT_TRUE(bytes.outOfRange()) << kind;
  }
}

TEST(FontBytes, ReadCopiesAllOrNothing) {
  FontBytes bytes(kData, sizeof kData);
  uint8_t out[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  ASSERT_TRUE(bytes.read(2, 4, out));
  EXPECT_EQ(0x56, out[0]);
  EXPECT_EQ(0xBC, out[3]);
  uint8_t untouched[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_FALSE(bytes.read(3, 4, untouched));
  EXPECT_EQ(0xEE, untouched[0]);
  EXPECT_TRUE(bytes.outOfRange());
  EXPECT_TRUE(bytes.read(6, 0, untouched));  // nothing, at the very end
}

TEST(FontBytes, ClearErrorsForgetsTheFlag) {
  FontBytes bytes(kData, sizeof kData);
  bytes.u8(99);
  ASSERT_TRUE(bytes.outOfRange());
  bytes.clearErrors();
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_EQ(0x12, bytes.u8(0));
}

TEST(FontBytes, AnEmptySourceReadsNothing) {
  FontBytes bytes;
  EXPECT_EQ(0u, bytes.size());
  EXPECT_EQ(0, bytes.u8(0));
  EXPECT_TRUE(bytes.outOfRange());
}
