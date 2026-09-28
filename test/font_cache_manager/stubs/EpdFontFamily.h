#pragma once

#include <cstdint>

struct EpdFontData {
  const void* groups = nullptr;
};

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };

  EpdFontFamily() = default;
  EpdFontFamily(const EpdFontData* regular, const EpdFontData* bold, const EpdFontData* italic,
                const EpdFontData* boldItalic)
      : styleData{regular, bold, italic, boldItalic} {}

  const EpdFontData* getData(Style style) const {
    const uint8_t requested = static_cast<uint8_t>(style) & 0x03;
    if (styleData[requested]) return styleData[requested];
    if ((requested & BOLD) && styleData[BOLD]) return styleData[BOLD];
    if ((requested & ITALIC) && styleData[ITALIC]) return styleData[ITALIC];
    return styleData[REGULAR];
  }

 private:
  const EpdFontData* styleData[4] = {};
};
