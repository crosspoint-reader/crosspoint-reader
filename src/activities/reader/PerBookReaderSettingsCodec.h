#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "PerBookReaderSettings.h"

namespace PerBookReaderSettingsCodec {

constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'R', 'S'};
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t AUTO_TURN_VERSION = 2;
constexpr uint8_t VERSION = 3;
constexpr uint16_t LEGACY_PAYLOAD_SIZE = 46;
constexpr uint16_t PAYLOAD_SIZE = 48;
constexpr size_t VERSION_OFFSET = MAGIC.size();
constexpr size_t PAYLOAD_LENGTH_OFFSET = VERSION_OFFSET + 1;
constexpr size_t CRC_OFFSET = PAYLOAD_LENGTH_OFFSET + 2;
constexpr size_t PAYLOAD_OFFSET = CRC_OFFSET + 4;
constexpr size_t ENCODED_SIZE = PAYLOAD_OFFSET + PAYLOAD_SIZE;
constexpr size_t LEGACY_ENCODED_SIZE = PAYLOAD_OFFSET + LEGACY_PAYLOAD_SIZE;
static_assert(ENCODED_SIZE == 59);
static_assert(LEGACY_ENCODED_SIZE == 57);

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

inline bool isSupportedAutoPageTurnSeconds(const uint8_t seconds) { return seconds >= 5 && seconds <= 120; }

inline bool isValidUtf8(const std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    if (first <= 0x7F) {
      if (first == 0) return false;
      ++i;
      continue;
    }
    size_t continuationCount = 0;
    uint32_t codePoint = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
      codePoint = first & 0x1FU;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
      codePoint = first & 0x0FU;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
      codePoint = first & 0x07U;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (continuationCount > text.size() - i - 1) return false;
    for (size_t j = 1; j <= continuationCount; ++j) {
      const uint8_t continuation = static_cast<uint8_t>(text[i + j]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      codePoint = (codePoint << 6) | (continuation & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) return false;
    i += continuationCount + 1;
  }
  return true;
}

inline bool hasCanonicalSdFontName(const std::array<char, PerBookReaderSettings::SD_FONT_NAME_CAPACITY>& name) {
  size_t length = 0;
  while (length < name.size() && name[length] != '\0') ++length;
  return length < name.size() && isValidUtf8({name.data(), length}) &&
         std::all_of(
             name.begin() + static_cast<std::ptrdiff_t>(length), name.end(),
             [](const char value) { return value == '\0'; });
}

inline bool isValid(const PerBookReaderSettings& settings) {
  const auto isToggle = [](const uint8_t value) { return value <= 1; };
  return settings.fontFamily < 2 && settings.fontSize < 4 && settings.lineSpacing < 3 &&
         settings.paragraphAlignment < 5 && settings.orientation < 4 && settings.screenMargin >= 5 &&
         settings.screenMargin <= 40 && isToggle(settings.embeddedStyle) && isToggle(settings.focusReadingEnabled) &&
         isToggle(settings.hyphenationEnabled) && isToggle(settings.extraParagraphSpacing) &&
         isToggle(settings.textAntiAliasing) && settings.imageRendering < 3 &&
         isToggle(settings.forceParagraphIndents) &&
         isValidEpubRenderMode(static_cast<uint8_t>(settings.renderMode)) &&
         (settings.hasRenderModeOverride || settings.renderMode == EpubRenderMode::Balanced) &&
         ((!settings.hasAutoPageTurnInterval && settings.autoPageTurnSeconds == 0 &&
           !settings.autoPageTurnStartsOnOpen) ||
          (settings.hasAutoPageTurnInterval && isSupportedAutoPageTurnSeconds(settings.autoPageTurnSeconds))) &&
         hasCanonicalSdFontName(settings.sdFontFamilyName);
}

inline bool encode(const PerBookReaderSettings& settings, Encoded& encoded) {
  if (!isValid(settings)) return false;

  encoded.fill(0);
  std::memcpy(encoded.data(), MAGIC.data(), MAGIC.size());
  encoded[VERSION_OFFSET] = VERSION;
  writeU16(encoded.data() + PAYLOAD_LENGTH_OFFSET, PAYLOAD_SIZE);

  uint8_t* payload = encoded.data() + PAYLOAD_OFFSET;
  payload[0] =
      static_cast<uint8_t>((settings.hasReaderOverrides ? 1U : 0U) | (settings.hasAutoPageTurnInterval ? 2U : 0U) |
                           (settings.autoPageTurnStartsOnOpen ? 4U : 0U) |
                           (settings.hasRenderModeOverride ? 8U : 0U) | (settings.safeModeEnabled ? 16U : 0U));
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
  payload[13] = settings.autoPageTurnSeconds;
  std::memcpy(payload + 14, settings.sdFontFamilyName.data(), settings.sdFontFamilyName.size());
  payload[46] = settings.forceParagraphIndents;
  payload[47] = static_cast<uint8_t>(settings.renderMode);

  writeU32(encoded.data() + CRC_OFFSET, crc32(payload, PAYLOAD_SIZE));
  return true;
}

inline DecodeStatus decode(const uint8_t* data, const size_t length, PerBookReaderSettings& settings) {
  if (length < VERSION_OFFSET + 1) return DecodeStatus::TRUNCATED;
  if (std::memcmp(data, MAGIC.data(), MAGIC.size()) != 0) return DecodeStatus::BAD_MAGIC;

  const uint8_t version = data[VERSION_OFFSET];
  if (version > VERSION) return DecodeStatus::NEWER_VERSION;
  if (version != LEGACY_VERSION && version != AUTO_TURN_VERSION && version != VERSION) {
    return DecodeStatus::UNSUPPORTED_VERSION;
  }
  const uint16_t payloadSize = version == VERSION ? PAYLOAD_SIZE : LEGACY_PAYLOAD_SIZE;
  const size_t encodedSize = PAYLOAD_OFFSET + payloadSize;
  if (length < encodedSize) return DecodeStatus::TRUNCATED;
  if (length > encodedSize) return DecodeStatus::WRONG_SIZE;
  if (readU16(data + PAYLOAD_LENGTH_OFFSET) != payloadSize) return DecodeStatus::BAD_PAYLOAD_LENGTH;

  const uint8_t* payload = data + PAYLOAD_OFFSET;
  if (readU32(data + CRC_OFFSET) != crc32(payload, payloadSize)) return DecodeStatus::BAD_CRC;
  const uint8_t allowedFlags = version == LEGACY_VERSION ? 0x03U : (version == AUTO_TURN_VERSION ? 0x07U : 0x1FU);
  if ((payload[0] & ~allowedFlags) != 0) return DecodeStatus::INVALID_VALUE;

  PerBookReaderSettings decoded;
  decoded.hasReaderOverrides = (payload[0] & 0x01U) != 0;
  decoded.hasAutoPageTurnInterval = (payload[0] & 0x02U) != 0;
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
  if (version == LEGACY_VERSION) {
    switch (payload[13]) {
      case 1:
        decoded.autoPageTurnSeconds = 60;
        break;
      case 3:
        decoded.autoPageTurnSeconds = 20;
        break;
      case 6:
        decoded.autoPageTurnSeconds = 10;
        break;
      case 12:
        decoded.autoPageTurnSeconds = 5;
        break;
      default:
        decoded.autoPageTurnSeconds = payload[13];
        break;
    }
    decoded.autoPageTurnStartsOnOpen = decoded.hasAutoPageTurnInterval;
  } else {
    decoded.autoPageTurnSeconds = payload[13];
    decoded.autoPageTurnStartsOnOpen = (payload[0] & 0x04U) != 0;
  }
  std::memcpy(decoded.sdFontFamilyName.data(), payload + 14, decoded.sdFontFamilyName.size());
  if (version == VERSION) {
    decoded.hasRenderModeOverride = (payload[0] & 0x08U) != 0;
    decoded.safeModeEnabled = (payload[0] & 0x10U) != 0;
    decoded.forceParagraphIndents = payload[46];
    decoded.renderMode = static_cast<EpubRenderMode>(payload[47]);
  }

  if (!isValid(decoded)) return DecodeStatus::INVALID_VALUE;
  settings = decoded;
  return DecodeStatus::OK;
}

}  // namespace PerBookReaderSettingsCodec
