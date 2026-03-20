#pragma once

#include <InflateReader.h>

#include <vector>

#include "EpdFontData.h"

// When defined, glyph groups are stored in packed format in flash (no row-padding).
// The decompressor uses a 4-slot LRU cache and returns direct pointers into
// decompressed group buffers, avoiding per-glyph compaction overhead.
// Font headers must be generated without --byte-align when this is enabled.
// When not defined, groups use byte-aligned (row-padded) format for better DEFLATE
// ratios, with a single hot-group slot and per-glyph compaction at render time.
#define FONT_PACKED_GROUPS

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // With FONT_PACKED_GROUPS: checks page buffer, then 4-slot LRU cache.
  // Without: checks page buffer, then single hot-group with compaction.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data.
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
    uint32_t pageBufferBytes = 0;
    uint32_t pageGlyphsBytes = 0;
    uint32_t hotGroupBytes = 0;
    uint32_t peakTempBytes = 0;
    uint32_t getBitmapTimeUs = 0;
    uint32_t getBitmapCalls = 0;
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer: flat array of prewarmed glyph bitmaps with sorted lookup
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
#ifndef FONT_PACKED_GROUPS
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
#endif
  };
  uint8_t* pageBuffer = nullptr;
  const EpdFontData* pageFont = nullptr;
  PageGlyphEntry* pageGlyphs = nullptr;
  uint16_t pageGlyphCount = 0;

  void freePageBuffer();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);

#ifdef FONT_PACKED_GROUPS
  // 4-slot LRU cache: each slot holds a decompressed group in packed format.
  // getBitmap returns a direct pointer into the cache buffer (no compaction).
  static constexpr uint8_t CACHE_SLOTS = 4;
  struct CacheEntry {
    const EpdFontData* font = nullptr;
    uint16_t groupIndex = 0;
    uint8_t* data = nullptr;
    uint32_t dataSize = 0;
    uint32_t lastUsed = 0;
    bool valid = false;
  };
  CacheEntry cache[CACHE_SLOTS] = {};
  uint32_t accessCounter = 0;

  void freeAllEntries();
  CacheEntry* findInCache(const EpdFontData* fontData, uint16_t groupIndex);
  CacheEntry* findEvictionCandidate();
#else
  // Single hot-group slot with byte-aligned data.
  // Each getBitmap call compacts the requested glyph into a scratch buffer.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  std::vector<uint8_t> hotGroup;
  std::vector<uint8_t> hotGlyphBuf;

  void freeHotGroup();
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
#endif
};
