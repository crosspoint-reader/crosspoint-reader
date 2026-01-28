#include "CrossPointFont.h"

#include <Utf8.h>

#include <algorithm>
#include <cmath>

#define FONT_SCALE 2

namespace {
// Number of set bits from 0->15
uint8_t bitCount[] = {
    0,  // 0b0000,
    1,  // 0b0001,
    1,  // 0b0010,
    2,  // 0b0011,
    1,  // 0b0100,
    2,  // 0b0101,
    2,  // 0b0110,
    3,  // 0b0111,
    1,  // 0b1000,
    2,  // 0b1001,
    2,  // 0b1010,
    3,  // 0b1011,
    2,  // 0b1100,
    3,  // 0b1101,
    3,  // 0b1110,
    4,  // 0b1111,
};
}  // namespace

void CrossPointFont::getTextBounds(const char* string, const Style style, const int startX, const int startY, int* minX,
                                   int* minY, int* maxX, int* maxY) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (*string == '\0') {
    return;
  }

  int cursorX = startX;
  const int cursorY = startY;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const CrossPointFontGlyph* glyph = getGlyph(cp, style);

    if (!glyph) {
      glyph = getGlyph(REPLACEMENT_GLYPH, style);
    }

    if (!glyph) {
      // TODO: Better handle this?
      continue;
    }

    *minX = std::min(*minX, cursorX + glyph->xOffset / FONT_SCALE);
    *maxX = std::max(*maxX, cursorX + (glyph->xOffset + glyph->width) / FONT_SCALE);
    *minY = std::min(*minY, cursorY + (glyph->yOffset - glyph->height) / FONT_SCALE);
    *maxY = std::max(*maxY, cursorY + glyph->yOffset / FONT_SCALE);
    cursorX += glyph->xAdvance / FONT_SCALE;
  }
}

void CrossPointFont::getTextDimensions(const char* string, const Style style, int* w, int* h) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, style, 0, 0, &minX, &minY, &maxX, &maxY);

  *w = maxX - minX;
  *h = maxY - minY;
}

uint8_t CrossPointFont::styleGroup(const Style style) const {
  if (style == REGULAR) return 0;

  if (data.header.styles == 0b0001) {
    // Only have regular font, show regular
    return 0;
  }
  if (data.header.styles == 0b0011) {
    // Only have bold and regular font
    // Show bold if style is bold or bold_italic
    return style == BOLD || style == BOLD_ITALIC ? 1 : 0;
  }
  if (data.header.styles == 0b0101) {
    // Only have italic and regular font
    // Show italic if style is italic or bold_italic
    return style == ITALIC || style == BOLD_ITALIC ? 1 : 0;
  }
  if (data.header.styles == 0b1001) {
    // Only have bold_italic and regular font
    // Show bold_italic if style is any non-regular
    return style == BOLD_ITALIC || style == BOLD || style == ITALIC ? 1 : 0;
  }
  if (data.header.styles == 0b0111) {
    // Have all but bold_italic
    // Show bold if style is bold_italic, otherwise show the requested style
    return style == BOLD_ITALIC ? 1 : style;
  }
  if (data.header.styles == 0b1011) {
    // Have all but italic
    // Show bold_italic if style is italic, otherwise show the requested style
    return style == ITALIC ? 2 : style;
  }
  if (data.header.styles == 0b1101) {
    // Have all but bold
    // Show bold_italic if style is bold, otherwise show the requested style
    return style == BOLD ? 2 : style;
  }
  if (data.header.styles == 0b1111) {
    return style;
  }

  return 0;
}

const CrossPointFontGlyph* CrossPointFont::getGlyph(const uint32_t cp, const Style style) const {
  const CrossPointFontUnicodeInterval* intervals = data.intervals;
  const int count = data.header.intervalCount;

  if (count == 0) return nullptr;

  // Binary search for O(log n) lookup instead of O(n)
  // Critical for Korean fonts with many unicode intervals
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const CrossPointFontUnicodeInterval* interval = &intervals[mid];

    if (cp < interval->first) {
      right = mid - 1;
    } else if (cp > interval->last) {
      left = mid + 1;
    } else {
      // Found: cp >= interval->first && cp <= interval->last
      const uint32_t index =
          interval->offset + (cp - interval->first) * bitCount[data.header.styles] + styleGroup(style);
      return &data.glyphs[index];
    }
  }

  return nullptr;
}
