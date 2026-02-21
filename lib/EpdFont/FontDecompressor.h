#pragma once

#include <uzlib.h>

#include <cstdint>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  struct uzlib_uncomp decomp = {};

  // Page buffer: flat array of prewarmed glyph bitmaps with sorted lookup
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  uint8_t* pageBuffer = nullptr;
  const EpdFontData* pageFont = nullptr;
  PageGlyphEntry* pageGlyphs = nullptr;
  uint16_t pageGlyphCount = 0;

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  uint8_t* hotGroupData = nullptr;
  uint32_t hotGroupSize = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call.
  uint8_t* hotGlyphBuf = nullptr;
  uint16_t hotGlyphBufSize = 0;

  void freePageBuffer();
  void freeHotGroup();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
