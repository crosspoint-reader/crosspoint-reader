#pragma once

#include <cstdint>

#include "EpdFontFamily.h"

class FontDecompressor {
 public:
  void clearCache() {}
  int prewarmCache(const EpdFontData*, const char*) { return 0; }
  const uint8_t* getBitmap(const EpdFontData*, const EpdGlyph*, uint32_t) { return nullptr; }
  void logStats(const char*) {}
  void resetStats() {}
};
