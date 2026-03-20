#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdlib>

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
#ifdef FONT_PACKED_GROUPS
  freeAllEntries();
#else
  freeHotGroup();
#endif
}

void FontDecompressor::clearCache() {
  freePageBuffer();
#ifdef FONT_PACKED_GROUPS
  freeAllEntries();
#else
  freeHotGroup();
#endif
}

void FontDecompressor::freePageBuffer() {
  free(pageBuffer);
  pageBuffer = nullptr;
  free(pageGlyphs);
  pageGlyphs = nullptr;
  pageFont = nullptr;
  pageGlyphCount = 0;
}

#ifdef FONT_PACKED_GROUPS

void FontDecompressor::freeAllEntries() {
  for (auto& entry : cache) {
    if (entry.data) {
      free(entry.data);
      entry.data = nullptr;
    }
    entry.valid = false;
  }
  accessCounter = 0;
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
  for (auto& entry : cache) {
    if (!entry.valid) return &entry;
  }
  CacheEntry* lru = &cache[0];
  for (auto& entry : cache) {
    if (entry.lastUsed < lru->lastUsed) lru = &entry;
  }
  return lru;
}

#else  // byte-aligned hot group

void FontDecompressor::freeHotGroup() {
  hotGroup.clear();
  hotGroup.shrink_to_fit();
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  hotGlyphBuf.clear();
  hotGlyphBuf.shrink_to_fit();
}

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

#endif  // FONT_PACKED_GROUPS

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];
  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- getBitmap: page buffer then cache/hot-group fallback ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer first (populated by prewarmCache)
  if (pageBuffer && pageFont == fontData && pageGlyphCount > 0) {
    int left = 0, right = pageGlyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (pageGlyphs[mid].glyphIndex == glyphIndex) {
        if (pageGlyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &pageBuffer[pageGlyphs[mid].bufferOffset];
        }
        break;
      }
      if (pageGlyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
  }

  // Fallback path differs by mode
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

#ifdef FONT_PACKED_GROUPS
  // 4-slot LRU: check cache, decompress on miss, return direct pointer
  CacheEntry* entry = findInCache(fontData, groupIndex);
  if (entry) {
    stats.cacheHits++;
    entry->lastUsed = ++accessCounter;
    stats.getBitmapTimeUs += micros() - tStart;
    return &entry->data[glyph->dataOffset];
  }

  stats.cacheMisses++;
  entry = findEvictionCandidate();
  const EpdFontGroup& group = fontData->groups[groupIndex];

  if (entry->data) {
    free(entry->data);
    entry->data = nullptr;
  }
  entry->valid = false;

  auto* outBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
  if (!outBuf) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for group %u", group.uncompressedSize, groupIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  if (!decompressGroup(fontData, groupIndex, outBuf, group.uncompressedSize)) {
    free(outBuf);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  entry->font = fontData;
  entry->groupIndex = groupIndex;
  entry->data = outBuf;
  entry->dataSize = group.uncompressedSize;
  entry->valid = true;
  entry->lastUsed = ++accessCounter;
  stats.hotGroupBytes = group.uncompressedSize;
  stats.getBitmapTimeUs += micros() - tStart;
  return &entry->data[glyph->dataOffset];

#else
  // Single hot-group with compaction
  if (!(!hotGroup.empty() && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    hotGroup.resize(group.uncompressedSize);
    if (hotGroup.empty()) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup.data(), group.uncompressedSize)) {
      hotGroup.clear();
      hotGroup.shrink_to_fit();
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  if (glyph->dataLength > hotGlyphBuf.size()) {
    hotGlyphBuf.resize(glyph->dataLength);
  }
  if (hotGlyphBuf.empty()) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf.data(), glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf.data();
#endif
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;
  if (count == 0) return -1;

  int left = 0;
  int right = count - 1;
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];
    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }
  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  freePageBuffer();
  if (!fontData || !fontData->groups || !utf8Text) return 0;

  // Step 1: Collect unique glyph indices needed for this page
  uint32_t neededGlyphs[MAX_PAGE_GLYPHS];
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0) continue;

    bool found = false;
    for (uint16_t i = 0; i < glyphCount; i++) {
      if (neededGlyphs[i] == static_cast<uint32_t>(glyphIdx)) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (glyphCount < MAX_PAGE_GLYPHS) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(glyphIdx);
      } else if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use fallback", MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }

  if (glyphCount == 0) return 0;

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
    uint16_t gi = getGroupIndex(fontData, neededGlyphs[i]);
    bool found = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < 128) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use fallback");
        groupCapWarned = true;
      }
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Step 3: Allocate page buffer and lookup table
  pageBuffer = static_cast<uint8_t*>(malloc(totalBytes));
  pageGlyphs = static_cast<PageGlyphEntry*>(malloc(glyphCount * sizeof(PageGlyphEntry)));
  if (!pageBuffer || !pageGlyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    freePageBuffer();
    return glyphCount;
  }
  stats.pageBufferBytes = totalBytes;
  stats.pageGlyphsBytes = glyphCount * sizeof(PageGlyphEntry);

  pageFont = fontData;
  pageGlyphCount = glyphCount;

  // Initialize lookup entries
  for (uint16_t i = 0; i < glyphCount; i++) {
#ifdef FONT_PACKED_GROUPS
    pageGlyphs[i] = {neededGlyphs[i], UINT32_MAX};
#else
    pageGlyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
#endif
  }

  // Sort by glyphIndex for binary search in getBitmap()
  for (uint16_t i = 1; i < glyphCount; i++) {
    PageGlyphEntry key = pageGlyphs[i];
    int j = i - 1;
    while (j >= 0 && pageGlyphs[j].glyphIndex > key.glyphIndex) {
      pageGlyphs[j + 1] = pageGlyphs[j];
      j--;
    }
    pageGlyphs[j + 1] = key;
  }

#ifndef FONT_PACKED_GROUPS
  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  uint32_t groupAlignedTracker[128] = {};

  if (fontData->glyphToGroup) {
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      uint8_t gpPos = groupCount;
      for (uint8_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;

      const EpdGlyph& glyph = fontData->glyph[i];

      int left = 0, right = (int)pageGlyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (pageGlyphs[mid].glyphIndex == i) {
          pageGlyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (pageGlyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = (int)pageGlyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (pageGlyphs[mid].glyphIndex == glyphI) {
            pageGlyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (pageGlyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }
#endif  // !FONT_PACKED_GROUPS

  // Step 4: For each unique group, decompress to temp buffer and extract needed glyphs
  uint32_t writeOffset = 0;
  int missed = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      free(tempBuf);
      missed++;
      continue;
    }

    for (uint16_t i = 0; i < pageGlyphCount; i++) {
      if (pageGlyphs[i].bufferOffset != UINT32_MAX) continue;
      if (getGroupIndex(fontData, pageGlyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[pageGlyphs[i].glyphIndex];

#ifdef FONT_PACKED_GROUPS
      // Packed format: copy directly from decompressed group using dataOffset
      memcpy(&pageBuffer[writeOffset], &tempBuf[glyph.dataOffset], glyph.dataLength);
#else
      // Byte-aligned format: compact from row-padded into packed
      compactSingleGlyph(&tempBuf[pageGlyphs[i].alignedOffset], &pageBuffer[writeOffset], glyph.width, glyph.height);
#endif
      pageGlyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }

    free(tempBuf);
  }

  LOG_DBG("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
