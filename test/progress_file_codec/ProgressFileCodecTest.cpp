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

TEST(ProgressFileCodec, TxtV2RoundTripsByteOffsetWithMagicAndVersion) {
  uint8_t data[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(0x89ABCDEF, data);

  EXPECT_EQ(data[0], ProgressFileCodec::TXT_MAGIC);
  EXPECT_EQ(data[1], ProgressFileCodec::TXT_VERSION);
  uint32_t offset = 0;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(data, sizeof(data), offset), ProgressFileCodec::TxtDecodeStatus::Ok);
  EXPECT_EQ(offset, 0x89ABCDEFu);
}

TEST(ProgressFileCodec, TxtDecoderDistinguishesLegacyAndNewerRecords) {
  uint8_t legacy[4];
  ProgressFileCodec::encodePage(17, legacy);
  uint32_t value = 0;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(legacy, sizeof(legacy), value),
            ProgressFileCodec::TxtDecodeStatus::LegacyPage);
  EXPECT_EQ(value, 17u);

  uint8_t newer[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(42, newer);
  newer[1] = ProgressFileCodec::TXT_VERSION + 1;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(newer, sizeof(newer), value),
            ProgressFileCodec::TxtDecodeStatus::NewerVersion);

  newer[0] ^= 0x80U;
  EXPECT_EQ(ProgressFileCodec::decodeTxt(newer, sizeof(newer), value), ProgressFileCodec::TxtDecodeStatus::BadMagic);
}
