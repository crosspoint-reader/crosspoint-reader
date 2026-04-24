#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffers).
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
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm or getBitmap miss
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
    uint16_t groupIndex;     // cached to avoid re-calling getGroupIndex in prewarm Step 4
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  static constexpr uint16_t HOT_GLYPH_BUF_SIZE = 512;  // largest packed single glyph

  // Scratch buffer for compacting a single glyph after a getBitmap() miss.
  // Valid until the next getBitmap() call.
  uint8_t _hotGlyphBuf[HOT_GLYPH_BUF_SIZE] = {};

  void freePageBuffer();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
