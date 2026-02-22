#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Valid until LRU eviction (safe for the duration of one glyph render).
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint16_t glyphIndex);

  // Evict all cached decompressed groups (call between pages for within-page-only caching).
  void clearCache();

 private:
  static constexpr uint8_t CACHE_SLOTS = 4;

  struct CacheEntry {
    const EpdFontData* font = nullptr;
    uint16_t groupIndex = 0;
    uint8_t* data = nullptr;
    uint32_t dataSize = 0;
    uint32_t lastUsed = 0;
    bool valid = false;
  };

  InflateReader inflateReader;
  CacheEntry cache[CACHE_SLOTS] = {};
  uint32_t accessCounter = 0;

  void freeAllEntries();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint16_t glyphIndex);
  CacheEntry* findInCache(const EpdFontData* fontData, uint16_t groupIndex);
  CacheEntry* findEvictionCandidate();
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, CacheEntry* entry);
};
