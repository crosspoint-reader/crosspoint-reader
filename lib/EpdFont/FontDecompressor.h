#pragma once

#include <InflateReader.h>

#include <memory>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Start a prewarm generation and release only page-transient buffers. The
  // packed GlyphStream LRU survives until clearCache()/deinit().
  void beginPrewarm();
  void clearTransientCache();

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
    uint16_t prewarmGlyphs = 0;
    uint32_t prewarmGlyphStreamPixels = 0;
    uint16_t prewarmGlyphCacheHits = 0;
    uint16_t prewarmGlyphCacheMisses = 0;
    uint32_t prewarmGlyphCacheHitPixels = 0;
#if defined(GLYPH_STREAM_PERF_METRICS)
    uint32_t prewarmGlyphStreamDecodedPixels = 0;
#endif
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t glyphCacheBytes = 0;  // packed bytes resident in the persistent LRU
    uint16_t glyphCacheEntries = 0;
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
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Packed GlyphStream LRU. Entries use a compact (font-slot, uint16 glyph)
  // key so the fixed metadata table is 2 KiB on ESP32-C3. Built-in fonts are
  // well below 65536 glyphs; oversized indices fall back to the page cache.
  static constexpr uint16_t GLYPH_CACHE_DATA_BYTES = 8 * 1024;
  static constexpr uint16_t MAX_GLYPH_CACHE_ENTRIES = 256;
  static constexpr uint8_t MAX_GLYPH_CACHE_FONTS = MAX_PAGE_SLOTS;
  struct GlyphCacheEntry {
    uint32_t key;
    uint16_t bufferOffset;
    uint16_t lastUsed;
  };
  struct GlyphCacheFont {
    const EpdFontData* fontData = nullptr;
    uint16_t lastUsed = 0;
  };
  static_assert(sizeof(GlyphCacheEntry) == 8);

  std::unique_ptr<uint8_t[]> glyphCacheBuffer;
  std::unique_ptr<GlyphCacheEntry[]> glyphCacheEntries;
  GlyphCacheFont glyphCacheFonts[MAX_GLYPH_CACHE_FONTS] = {};
  uint16_t glyphCacheEntryCount = 0;
  uint16_t glyphCacheUsedBytes = 0;
  uint16_t glyphCacheGeneration = 0;

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  // Nothrow high-water malloc buffers, NOT std::vector: getBitmap() runs on the render path,
  // and under -fno-exceptions a vector resize that hits OOM abort()s the firmware instead of
  // failing (field crash: hotGroup.resize() -> std::bad_alloc -> abort with ~11 KB free).
  // ensureCapacity() returns false on OOM so the caller can skip the glyph gracefully.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  uint8_t* hotGroup = nullptr;  // owned; freed in freeHotGroup()/dtor
  uint32_t hotGroupCapacity = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as hotGroup.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

  // Grow (never shrink) an owned buffer to at least `needed` bytes; false on OOM, buffer freed.
  static bool ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed);

  void freePageBuffer();
  void freeHotGroup();
  bool ensureGlyphCache();
  int8_t findGlyphCacheFont(const EpdFontData* fontData) const;
  int8_t acquireGlyphCacheFont(const EpdFontData* fontData);
  int findGlyphCacheEntry(uint32_t key) const;
  uint16_t glyphCacheEntrySize(const GlyphCacheEntry& entry) const;
  void eraseGlyphCacheEntry(uint16_t entryIndex);
  void compactGlyphCache();
  bool prewarmGlyphStreamLru(const EpdFontData* fontData, const uint32_t* neededGlyphs, uint16_t glyphCount,
                             int& missed);
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
