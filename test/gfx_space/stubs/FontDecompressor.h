#pragma once

#include <EpdFontData.h>

class FontDecompressor {
 public:
  const uint8_t* getBitmap(const EpdFontData*, const EpdGlyph*, uint32_t);
};
