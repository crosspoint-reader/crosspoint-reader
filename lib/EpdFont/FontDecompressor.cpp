#include "FontDecompressor.h"

#include <HardwareSerial.h>
#include <miniz.h>

#include <cstdlib>
#include <cstring>

bool FontDecompressor::init() {
  decompressor = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
  if (!decompressor) {
    Serial.printf("[%lu] [FDC] Failed to allocate tinfl_decompressor\n", millis());
    return false;
  }
  memset(decompressor, 0, sizeof(tinfl_decompressor));
  return true;
}

void FontDecompressor::deinit() {
  for (auto& entry : cache) {
    if (entry.data) {
      free(entry.data);
      entry.data = nullptr;
    }
    entry.valid = false;
  }
  if (decompressor) {
    free(decompressor);
    decompressor = nullptr;
  }
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint16_t glyphIndex) {
  uint16_t baseSize = fontData->groups[0].glyphCount;
  if (glyphIndex < baseSize) return 0;
  return 1 + (glyphIndex - baseSize) / 8;
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
    Serial.printf("[%lu] [FDC] Failed to allocate %u bytes for group %u\n", millis(), group.uncompressedSize,
                  groupIndex);
    return false;
  }

  // Decompress
  tinfl_init(decompressor);
  size_t inBytes = group.compressedSize;
  size_t outBytes = group.uncompressedSize;
  const uint8_t* inputBuf = &fontData->bitmap[group.compressedOffset];

  tinfl_status status =
      tinfl_decompress(decompressor, inputBuf, &inBytes, nullptr, outBuf, &outBytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

  if (status != TINFL_STATUS_DONE) {
    Serial.printf("[%lu] [FDC] Decompression failed for group %u (status %d)\n", millis(), groupIndex, status);
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
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);

  // Check cache
  CacheEntry* entry = findInCache(fontData, groupIndex);
  if (entry) {
    entry->lastUsed = ++accessCounter;
    return &entry->data[glyph->dataOffset];
  }

  // Cache miss - decompress
  entry = findEvictionCandidate();
  if (!decompressGroup(fontData, groupIndex, entry)) {
    return nullptr;
  }

  entry->lastUsed = ++accessCounter;
  return &entry->data[glyph->dataOffset];
}
