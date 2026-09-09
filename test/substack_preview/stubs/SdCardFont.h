#pragma once

// Host-build stand-in for lib/EpdFont/SdCardFont.h. substack_preview never
// registers an SD-card font (sdCardFonts_ stays empty in GfxRenderer), so
// these bodies are never exercised — they only need to exist, with matching
// signatures, for the linker. EpdFontData.h (real, portable) supplies the
// real EpdGlyph type rather than a forward declaration, since the real one
// is a `typedef struct {...} EpdGlyph;` (conflicts with `struct EpdGlyph;`).
#include <cstdint>
#include <deque>
#include <string>

#include "EpdFontData.h"

class SdCardFont {
 public:
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);

  static SdCardFont* fromMissCtx(void*) { return nullptr; }

  int prewarm(const char*, uint8_t = 0x0F, bool = false, bool = true) { return 0; }
  int prewarm(TextGetter, const void*, uint32_t, uint8_t = 0x0F, bool = false, bool = true) { return 0; }
  bool isOverflowGlyph(const EpdGlyph*) const { return false; }
  const uint8_t* getOverflowBitmap(const EpdGlyph*) const { return nullptr; }

  int buildAdvanceTable(const char*, uint8_t = 0x0F, const char* = nullptr) { return 0; }
  int buildAdvanceTable(const std::deque<std::string>&, bool, uint8_t = 0x0F, const char* = nullptr) { return 0; }
  uint16_t getAdvance(uint32_t, uint8_t) const { return 0; }
  bool hasAdvanceTable() const { return false; }

  void clearCache() {}
  void releaseResidentCaches() {}
  uint8_t resolveStyle(uint8_t style) const { return style; }
  uint8_t resolveStyleMask(uint8_t styleMask) const { return styleMask; }
  void logStats(const char* = "SDCF") {}
  void resetStats() {}
};
