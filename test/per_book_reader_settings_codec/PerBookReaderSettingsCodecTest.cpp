#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "PerBookReaderSettingsCodec.h"

namespace {

using namespace PerBookReaderSettingsCodec;

PerBookReaderSettings populatedSettings() {
  PerBookReaderSettings settings;
  settings.hasReaderOverrides = true;
  settings.hasAutoPageTurnRate = true;
  settings.fontFamily = 1;
  settings.fontSize = 3;
  settings.lineSpacing = 2;
  settings.paragraphAlignment = 4;
  settings.orientation = 3;
  settings.screenMargin = 40;
  settings.embeddedStyle = 0;
  settings.focusReadingEnabled = 1;
  settings.hyphenationEnabled = 1;
  settings.extraParagraphSpacing = 0;
  settings.textAntiAliasing = 0;
  settings.imageRendering = 2;
  settings.autoPageTurnRate = 12;
  setPerBookSdFontFamilyName(settings, "Noto Sans VN");
  return settings;
}

void refreshCrc(Encoded& encoded) {
  writeU32(encoded.data() + CRC_OFFSET, crc32(encoded.data() + PAYLOAD_OFFSET, PAYLOAD_SIZE));
}

}  // namespace

TEST(PerBookReaderSettingsCodec, RoundTripsAllFields) {
  const auto expected = populatedSettings();
  Encoded encoded;
  ASSERT_TRUE(encode(expected, encoded));

  PerBookReaderSettings decoded;
  EXPECT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
  EXPECT_EQ(decoded, expected);
}

TEST(PerBookReaderSettingsCodec, UsesStableExactByteLayout) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));

  const Encoded expected = {0x43, 0x56, 0x52, 0x53, 0x01, 0x2E, 0x00, 0x75, 0x44, 0x31, 0x3F, 0x03, 0x01, 0x03, 0x02,
                            0x04, 0x03, 0x28, 0x00, 0x01, 0x01, 0x00, 0x00, 0x02, 0x0C, 0x4E, 0x6F, 0x74, 0x6F, 0x20,
                            0x53, 0x61, 0x6E, 0x73, 0x20, 0x56, 0x4E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(encoded, expected);
}

TEST(PerBookReaderSettingsCodec, RejectsTruncationAndTrailingBytes) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));
  PerBookReaderSettings decoded;

  EXPECT_EQ(decode(encoded.data(), encoded.size() - 1, decoded), DecodeStatus::TRUNCATED);
  std::array<uint8_t, ENCODED_SIZE + 1> extended{};
  std::copy(encoded.begin(), encoded.end(), extended.begin());
  EXPECT_EQ(decode(extended.data(), extended.size(), decoded), DecodeStatus::WRONG_SIZE);
}

TEST(PerBookReaderSettingsCodec, RejectsCorruptHeaderAndPayload) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));
  PerBookReaderSettings decoded;

  auto corrupt = encoded;
  corrupt[0] ^= 0x01;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::BAD_MAGIC);

  corrupt = encoded;
  corrupt[VERSION_OFFSET] = VERSION + 1;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::NEWER_VERSION);

  corrupt = encoded;
  corrupt[VERSION_OFFSET] = 0;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::UNSUPPORTED_VERSION);

  corrupt = encoded;
  corrupt[PAYLOAD_LENGTH_OFFSET] = PAYLOAD_SIZE - 1;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::BAD_PAYLOAD_LENGTH);

  corrupt = encoded;
  corrupt[PAYLOAD_OFFSET + 1] ^= 0x01;
  EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::BAD_CRC);
}

TEST(PerBookReaderSettingsCodec, RejectsOutOfRangeValuesWithValidCrc) {
  Encoded encoded;
  ASSERT_TRUE(encode(populatedSettings(), encoded));
  PerBookReaderSettings decoded;

  const std::array<std::pair<size_t, uint8_t>, 15> invalidValues = {
      std::pair{size_t{0}, uint8_t{0x80}}, std::pair{size_t{1}, uint8_t{2}},  std::pair{size_t{2}, uint8_t{4}},
      std::pair{size_t{3}, uint8_t{3}},    std::pair{size_t{4}, uint8_t{5}},  std::pair{size_t{5}, uint8_t{4}},
      std::pair{size_t{6}, uint8_t{41}},   std::pair{size_t{7}, uint8_t{2}},  std::pair{size_t{8}, uint8_t{2}},
      std::pair{size_t{9}, uint8_t{2}},    std::pair{size_t{10}, uint8_t{2}}, std::pair{size_t{11}, uint8_t{2}},
      std::pair{size_t{12}, uint8_t{3}},   std::pair{size_t{13}, uint8_t{2}}, std::pair{size_t{45}, uint8_t{'x'}},
  };
  for (const auto& [offset, value] : invalidValues) {
    auto corrupt = encoded;
    corrupt[PAYLOAD_OFFSET + offset] = value;
    refreshCrc(corrupt);
    EXPECT_EQ(decode(corrupt.data(), corrupt.size(), decoded), DecodeStatus::INVALID_VALUE) << offset;
  }

  auto invalid = populatedSettings();
  invalid.autoPageTurnRate = 2;
  EXPECT_FALSE(encode(invalid, encoded));
}

TEST(PerBookReaderSettingsCodec, DefaultsAndSdFontAreAlwaysTerminated) {
  PerBookReaderSettings defaults;
  EXPECT_FALSE(defaults.hasReaderOverrides);
  EXPECT_FALSE(defaults.hasAutoPageTurnRate);
  EXPECT_EQ(defaults.autoPageTurnRate, 0);
  EXPECT_EQ(defaults.sdFontFamilyName.front(), '\0');
  EXPECT_EQ(defaults.sdFontFamilyName.back(), '\0');

  setPerBookSdFontFamilyName(defaults, std::string(80, 'x'));
  EXPECT_EQ(defaults.sdFontFamilyName.back(), '\0');
  EXPECT_EQ(std::char_traits<char>::length(defaults.sdFontFamilyName.data()),
            PerBookReaderSettings::SD_FONT_NAME_CAPACITY - 1);

  Encoded encoded;
  ASSERT_TRUE(encode(defaults, encoded));
  PerBookReaderSettings decoded;
  ASSERT_EQ(decode(encoded.data(), encoded.size(), decoded), DecodeStatus::OK);
  EXPECT_EQ(decoded.sdFontFamilyName.back(), '\0');
}
