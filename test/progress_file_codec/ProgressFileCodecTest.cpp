#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "ProgressFileCodec.h"

TEST(ProgressFileCodec, RoundTripsFullUint32Range) {
  for (const uint32_t page : std::array<uint32_t, 6>{0, 1, 0xFFFF, 0x10000, 0x80000000, 0xFFFFFFFF}) {
    uint8_t data[4];
    ProgressFileCodec::encodePage(page, data);
    EXPECT_EQ(ProgressFileCodec::decodePage(data), page);
  }
}

TEST(ProgressFileCodec, UsesLittleEndianLayout) {
  uint8_t data[4];
  ProgressFileCodec::encodePage(0x89ABCDEF, data);

  EXPECT_EQ(data[0], 0xEF);
  EXPECT_EQ(data[1], 0xCD);
  EXPECT_EQ(data[2], 0xAB);
  EXPECT_EQ(data[3], 0x89);
}
