#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdlib>
#include <cstring>

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  memset(&decomp, 0, sizeof(decomp));
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  free(pageBuffer);
  pageBuffer = nullptr;
  free(pageGlyphs);
  pageGlyphs = nullptr;
  pageFont = nullptr;
  pageGlyphCount = 0;
}

void FontDecompressor::freeHotGroup() {
  free(hotGroupData);
  hotGroupData = nullptr;
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  hotGroupSize = 0;
  free(hotGlyphBuf);
  hotGlyphBuf = nullptr;
  hotGlyphBufSize = 0;
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];
  const uint8_t* inputBuf = &fontData->bitmap[group.compressedOffset];

  const uint32_t tDecomp = millis();
  uzlib_uncompress_init(&decomp, NULL, 0);
  decomp.source = inputBuf;
  decomp.source_limit = inputBuf + group.compressedSize;
  decomp.dest_start = outBuf;
  decomp.dest = outBuf;
  decomp.dest_limit = outBuf + outSize;

  int res = uzlib_uncompress(&decomp);
  stats.decompressTimeMs += millis() - tDecomp;

  if (res < 0 || decomp.dest != decomp.dest_limit) {
    LOG_ERR("FDC", "Decompression failed for group %u (status %d)", groupIndex, res);
    return false;
  }
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
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

// --- getBitmap: page buffer → hot group → decompress ---

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
        stats.cacheHits++;
        stats.getBitmapTimeUs += micros() - tStart;
        return &pageBuffer[pageGlyphs[mid].bufferOffset];
      }
      if (pageGlyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(hotGroupData && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    free(hotGroupData);
    hotGroupData = static_cast<uint8_t*>(malloc(group.uncompressedSize));
    if (!hotGroupData) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      hotGroupSize = 0;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroupData, group.uncompressedSize)) {
      free(hotGroupData);
      hotGroupData = nullptr;
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      hotGroupSize = 0;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    hotGroupSize = group.uncompressedSize;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (glyph->dataLength > hotGlyphBufSize) {
    free(hotGlyphBuf);
    hotGlyphBuf = static_cast<uint8_t*>(malloc(glyph->dataLength));
    hotGlyphBufSize = hotGlyphBuf ? glyph->dataLength : 0;
  }
  if (!hotGlyphBuf) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroupData[alignedOff], hotGlyphBuf, glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf;
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
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

    // Deduplicate
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
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                 MAX_PAGE_GLYPHS);
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
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
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

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    pageGlyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
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

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      // Find this glyph's group position in neededGroups
      uint8_t gpPos = groupCount;
      for (uint8_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) { gpPos = j; break; }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted pageGlyphs to find if glyph i is needed
      int left = 0, right = (int)pageGlyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (pageGlyphs[mid].glyphIndex == i) {
          pageGlyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (pageGlyphs[mid].glyphIndex < i) left = mid + 1;
        else right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
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
          if (pageGlyphs[mid].glyphIndex < glyphI) left = mid + 1;
          else right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }

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

    // Extract needed glyphs directly from the byte-aligned temp buffer, compacting on the fly.
    // alignedOffset was pre-computed in step 3b — no full-group compact scan needed.
    for (uint16_t i = 0; i < pageGlyphCount; i++) {
      if (pageGlyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, pageGlyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[pageGlyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[pageGlyphs[i].alignedOffset], &pageBuffer[writeOffset],
                         glyph.width, glyph.height);
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
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs,
          stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label,
          stats.pageBufferBytes, stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label,
            stats.getBitmapCalls, stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
