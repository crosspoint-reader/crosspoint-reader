#pragma once

#include <cstdint>

#include "CrossPointFontFormat.h"
#include "Group5/Group5.h"

class CrossPointFont {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3 };

  CrossPointFontData data;
  explicit CrossPointFont(void* rawData) {
    data.header = *static_cast<CrossPointFontHeader*>(rawData);
    data.intervals = (CrossPointFontUnicodeInterval*)(static_cast<uint8_t*>(rawData) + sizeof(CrossPointFontHeader));
    data.glyphs = (CrossPointFontGlyph*)((uint8_t*)data.intervals +
                                         sizeof(CrossPointFontUnicodeInterval) * data.header.intervalCount);
    data.bitmap = (uint8_t*)data.glyphs + sizeof(CrossPointFontGlyph) * data.header.glyphCount;
  }

  ~CrossPointFont() = default;
  void getTextDimensions(const char* string, Style style, int* w, int* h) const;
  const CrossPointFontGlyph* getGlyph(uint32_t cp, Style style) const;

 private:
  void getTextBounds(const char* string, Style style, int startX, int startY, int* minX, int* minY, int* maxX,
                     int* maxY) const;
  uint8_t styleGroup(Style style) const;
};

// TODO: CrossPointFontSmall
