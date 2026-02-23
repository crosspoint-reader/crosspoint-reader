#include "FontDecompressor.h"

#include <Logging.h>

#include <cstdlib>

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::freeAllEntries() {
  for (auto& entry : cache) {
    if (entry.data) {
      free(entry.data);
      entry.data = nullptr;
    }
    entry.valid = false;
  }
}

void FontDecompressor::deinit() { freeAllEntries(); }

void FontDecompressor::clearCache() {
  freeAllEntries();
  accessCounter = 0;
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint16_t glyphIndex) {
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint16_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

FontDecompressor::CacheEntry* FontDecompressor::findInCache(const EpdFontData* fontData, uint16_t groupIndex) {
  for (auto& entry : cache) {
    if (entry.valid && entry.font == fontData && entry.groupIndex == groupIndex) {
      return &entry;
    }
  }
  return nullptr;
}

FontDecompressor::CacheEntry* FontDecompressor::findEvictionCandidate() {
  // Find an invalid slot first
  for (auto& entry : cache) {
    if (!entry.valid) {
      return &entry;
    }
  }
  // Otherwise evict LRU
  CacheEntry* lru = &cache[0];
  for (auto& entry : cache) {
    if (entry.lastUsed < lru->lastUsed) {
      lru = &entry;
    }
  }
  return lru;
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, CacheEntry* entry) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  // Free old buffer if reusing a slot
  if (entry->data) {
    free(entry->data);
    entry->data = nullptr;
  }
  entry->valid = false;

  // Allocate output buffer
  auto* outBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
  if (!outBuf) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for group %u", group.uncompressedSize, groupIndex);
    return false;
  }

  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, group.uncompressedSize)) {
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    free(outBuf);
    return false;
  }

  entry->font = fontData;
  entry->groupIndex = groupIndex;
  entry->data = outBuf;
  entry->dataSize = group.uncompressedSize;
  entry->valid = true;
  return true;
}

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint16_t glyphIndex) {
  if (!fontData->groups || fontData->groupCount == 0) {
    return &fontData->bitmap[glyph->dataOffset];
  }

  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    return nullptr;
  }

  // Check cache
  CacheEntry* entry = findInCache(fontData, groupIndex);
  if (entry) {
    entry->lastUsed = ++accessCounter;
    if (glyph->dataOffset + glyph->dataLength > entry->dataSize) {
      LOG_ERR("FDC", "dataOffset %u + dataLength %u out of bounds for group %u (size %u)", glyph->dataOffset,
              glyph->dataLength, groupIndex, entry->dataSize);
      return nullptr;
    }
    return &entry->data[glyph->dataOffset];
  }

  // Cache miss - decompress
  entry = findEvictionCandidate();
  if (!decompressGroup(fontData, groupIndex, entry)) {
    return nullptr;
  }

  entry->lastUsed = ++accessCounter;
  if (glyph->dataOffset + glyph->dataLength > entry->dataSize) {
    LOG_ERR("FDC", "dataOffset %u + dataLength %u out of bounds for group %u (size %u)", glyph->dataOffset,
            glyph->dataLength, groupIndex, entry->dataSize);
    return nullptr;
  }
  return &entry->data[glyph->dataOffset];
}
