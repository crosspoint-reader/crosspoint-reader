#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

struct PerBookReaderSettings {
  static constexpr size_t SD_FONT_NAME_CAPACITY = 32;

  bool hasReaderOverrides = false;
  bool hasAutoPageTurnRate = false;

  uint8_t fontFamily = 0;
  uint8_t fontSize = 1;
  uint8_t lineSpacing = 1;
  uint8_t paragraphAlignment = 0;
  uint8_t orientation = 0;
  uint8_t screenMargin = 5;
  uint8_t embeddedStyle = 1;
  uint8_t focusReadingEnabled = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  uint8_t imageRendering = 0;
  // Stored as pages per minute instead of a menu index so menu reordering cannot change its meaning.
  uint8_t autoPageTurnRate = 0;
  std::array<char, SD_FONT_NAME_CAPACITY> sdFontFamilyName{};

  bool operator==(const PerBookReaderSettings&) const = default;
};

inline void setPerBookSdFontFamilyName(PerBookReaderSettings& settings, const std::string_view name) {
  settings.sdFontFamilyName.fill('\0');
  const size_t len = std::min(name.size(), settings.sdFontFamilyName.size() - 1);
  std::memcpy(settings.sdFontFamilyName.data(), name.data(), len);
}
