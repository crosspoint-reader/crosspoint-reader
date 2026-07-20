#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "PerBookReaderSettings.h"

namespace PerBookReaderSettingsCodec {

constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'R', 'S'};
constexpr uint8_t VERSION = 1;
constexpr uint16_t PAYLOAD_SIZE = 46;
constexpr size_t VERSION_OFFSET = MAGIC.size();
constexpr size_t PAYLOAD_LENGTH_OFFSET = VERSION_OFFSET + 1;
constexpr size_t CRC_OFFSET = PAYLOAD_LENGTH_OFFSET + 2;
constexpr size_t PAYLOAD_OFFSET = CRC_OFFSET + 4;
constexpr size_t ENCODED_SIZE = PAYLOAD_OFFSET + PAYLOAD_SIZE;
static_assert(ENCODED_SIZE == 57);

using Encoded = std::array<uint8_t, ENCODED_SIZE>;

enum class DecodeStatus : uint8_t {
  OK,
  TRUNCATED,
  WRONG_SIZE,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  NEWER_VERSION,
  BAD_PAYLOAD_LENGTH,
  BAD_CRC,
  INVALID_VALUE,
};

inline uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

inline void writeU16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeU32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

inline uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

inline bool isSupportedAutoPageTurnRate(const uint8_t rate) {
  return rate == 1 || rate == 3 || rate == 6 || rate == 12;
}

inline bool hasCanonicalSdFontName(const std::array<char, PerBookReaderSettings::SD_FONT_NAME_CAPACITY>& name) {
  bool foundTerminator = false;
  for (const char c : name) {
    if (foundTerminator && c != '\0') return false;
    if (c == '\0') foundTerminator = true;
  }
  return foundTerminator;
}

inline bool isValid(const PerBookReaderSettings& settings) {
  const auto isToggle = [](const uint8_t value) { return value <= 1; };
  return settings.fontFamily < 2 && settings.fontSize < 4 && settings.lineSpacing < 3 &&
         settings.paragraphAlignment < 5 && settings.orientation < 4 && settings.screenMargin >= 5 &&
         settings.screenMargin <= 40 && isToggle(settings.embeddedStyle) && isToggle(settings.focusReadingEnabled) &&
         isToggle(settings.hyphenationEnabled) && isToggle(settings.extraParagraphSpacing) &&
         isToggle(settings.textAntiAliasing) && settings.imageRendering < 3 &&
         ((!settings.hasAutoPageTurnRate && settings.autoPageTurnRate == 0) ||
          (settings.hasAutoPageTurnRate && isSupportedAutoPageTurnRate(settings.autoPageTurnRate))) &&
         hasCanonicalSdFontName(settings.sdFontFamilyName);
}

inline bool encode(const PerBookReaderSettings& settings, Encoded& encoded) {
  if (!isValid(settings)) return false;

  encoded.fill(0);
  std::memcpy(encoded.data(), MAGIC.data(), MAGIC.size());
  encoded[VERSION_OFFSET] = VERSION;
  writeU16(encoded.data() + PAYLOAD_LENGTH_OFFSET, PAYLOAD_SIZE);

  uint8_t* payload = encoded.data() + PAYLOAD_OFFSET;
  payload[0] = static_cast<uint8_t>((settings.hasReaderOverrides ? 1U : 0U) | (settings.hasAutoPageTurnRate ? 2U : 0U));
  payload[1] = settings.fontFamily;
  payload[2] = settings.fontSize;
  payload[3] = settings.lineSpacing;
  payload[4] = settings.paragraphAlignment;
  payload[5] = settings.orientation;
  payload[6] = settings.screenMargin;
  payload[7] = settings.embeddedStyle;
  payload[8] = settings.focusReadingEnabled;
  payload[9] = settings.hyphenationEnabled;
  payload[10] = settings.extraParagraphSpacing;
  payload[11] = settings.textAntiAliasing;
  payload[12] = settings.imageRendering;
  payload[13] = settings.autoPageTurnRate;
  std::memcpy(payload + 14, settings.sdFontFamilyName.data(), settings.sdFontFamilyName.size());

  writeU32(encoded.data() + CRC_OFFSET, crc32(payload, PAYLOAD_SIZE));
  return true;
}

inline DecodeStatus decode(const uint8_t* data, const size_t length, PerBookReaderSettings& settings) {
  if (length < VERSION_OFFSET + 1) return DecodeStatus::TRUNCATED;
  if (std::memcmp(data, MAGIC.data(), MAGIC.size()) != 0) return DecodeStatus::BAD_MAGIC;

  const uint8_t version = data[VERSION_OFFSET];
  if (version > VERSION) return DecodeStatus::NEWER_VERSION;
  if (version != VERSION) return DecodeStatus::UNSUPPORTED_VERSION;
  if (length < ENCODED_SIZE) return DecodeStatus::TRUNCATED;
  if (length > ENCODED_SIZE) return DecodeStatus::WRONG_SIZE;
  if (readU16(data + PAYLOAD_LENGTH_OFFSET) != PAYLOAD_SIZE) return DecodeStatus::BAD_PAYLOAD_LENGTH;

  const uint8_t* payload = data + PAYLOAD_OFFSET;
  if (readU32(data + CRC_OFFSET) != crc32(payload, PAYLOAD_SIZE)) return DecodeStatus::BAD_CRC;
  if ((payload[0] & ~0x03U) != 0) return DecodeStatus::INVALID_VALUE;

  PerBookReaderSettings decoded;
  decoded.hasReaderOverrides = (payload[0] & 0x01U) != 0;
  decoded.hasAutoPageTurnRate = (payload[0] & 0x02U) != 0;
  decoded.fontFamily = payload[1];
  decoded.fontSize = payload[2];
  decoded.lineSpacing = payload[3];
  decoded.paragraphAlignment = payload[4];
  decoded.orientation = payload[5];
  decoded.screenMargin = payload[6];
  decoded.embeddedStyle = payload[7];
  decoded.focusReadingEnabled = payload[8];
  decoded.hyphenationEnabled = payload[9];
  decoded.extraParagraphSpacing = payload[10];
  decoded.textAntiAliasing = payload[11];
  decoded.imageRendering = payload[12];
  decoded.autoPageTurnRate = payload[13];
  std::memcpy(decoded.sdFontFamilyName.data(), payload + 14, decoded.sdFontFamilyName.size());

  if (!isValid(decoded)) return DecodeStatus::INVALID_VALUE;
  settings = decoded;
  return DecodeStatus::OK;
}

}  // namespace PerBookReaderSettingsCodec
