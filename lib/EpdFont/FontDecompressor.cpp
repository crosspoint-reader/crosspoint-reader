#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <array>
#include <cstdlib>
#include <cstring>

#include "GlyphStreamCodec.h"

namespace {

// FontDecompressor is a single global, non-reentrant service in firmware.
std::array<uint8_t, GlyphStreamCodec::SCRATCH_PLANE_SIZE> glyphStreamScratchA;
std::array<uint8_t, GlyphStreamCodec::SCRATCH_PLANE_SIZE> glyphStreamScratchB;

}  // namespace

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() { clearCache(); }

void FontDecompressor::clearCache() {
  clearTransientCache();
  glyphCacheBuffer.reset();
  glyphCacheEntries.reset();
  memset(glyphCacheFonts, 0, sizeof(glyphCacheFonts));
  glyphCacheEntryCount = 0;
  glyphCacheUsedBytes = 0;
  glyphCacheGeneration = 0;
}

void FontDecompressor::beginPrewarm() {
  if (++glyphCacheGeneration == 0) {
    glyphCacheGeneration = 1;
    for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) glyphCacheEntries[i].lastUsed = 0;
    for (auto& font : glyphCacheFonts) font.lastUsed = 0;
  }
}

void FontDecompressor::clearTransientCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    free(pageSlots[s].buffer);
    free(pageSlots[s].glyphs);
    pageSlots[s] = {};
  }
  pageSlotCount = 0;
}

void FontDecompressor::freeHotGroup() {
  free(hotGroup);
  hotGroup = nullptr;
  hotGroupCapacity = 0;
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  free(hotGlyphBuf);
  hotGlyphBuf = nullptr;
  hotGlyphBufCapacity = 0;
}

bool FontDecompressor::ensureGlyphCache() {
  if (glyphCacheBuffer && glyphCacheEntries) return true;

  auto buffer = makeUniqueNoThrow<uint8_t[]>(GLYPH_CACHE_DATA_BYTES);
  auto entries = makeUniqueNoThrow<GlyphCacheEntry[]>(MAX_GLYPH_CACHE_ENTRIES);
  if (!buffer || !entries) {
    LOG_ERR("FDC", "Failed to allocate GlyphStream LRU (%u data bytes, %u entries)", GLYPH_CACHE_DATA_BYTES,
            MAX_GLYPH_CACHE_ENTRIES);
    return false;
  }

  glyphCacheBuffer = std::move(buffer);
  glyphCacheEntries = std::move(entries);
  return true;
}

int8_t FontDecompressor::findGlyphCacheFont(const EpdFontData* fontData) const {
  for (uint8_t i = 0; i < MAX_GLYPH_CACHE_FONTS; ++i) {
    if (glyphCacheFonts[i].fontData == fontData) return static_cast<int8_t>(i);
  }
  return -1;
}

uint16_t FontDecompressor::glyphCacheEntrySize(const GlyphCacheEntry& entry) const {
  const uint8_t fontSlot = static_cast<uint8_t>(entry.key >> 16);
  if (fontSlot >= MAX_GLYPH_CACHE_FONTS || !glyphCacheFonts[fontSlot].fontData) return 0;
  const auto* fontData = glyphCacheFonts[fontSlot].fontData;
  const EpdGlyph& glyph = fontData->glyph[entry.key & 0xFFFFU];
  return static_cast<uint16_t>(GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit));
}

void FontDecompressor::eraseGlyphCacheEntry(const uint16_t entryIndex) {
  if (entryIndex >= glyphCacheEntryCount) return;
  glyphCacheUsedBytes -= glyphCacheEntrySize(glyphCacheEntries[entryIndex]);
  for (uint16_t i = entryIndex + 1; i < glyphCacheEntryCount; ++i) {
    glyphCacheEntries[i - 1] = glyphCacheEntries[i];
  }
  glyphCacheEntryCount--;
}

void FontDecompressor::compactGlyphCache() {
  uint16_t readCursor = 0;
  uint16_t writeOffset = 0;
  uint16_t nonEmptyEntries = 0;
  for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) {
    if (glyphCacheEntrySize(glyphCacheEntries[i]) > 0) nonEmptyEntries++;
  }

  for (uint16_t processed = 0; processed < nonEmptyEntries; ++processed) {
    int next = -1;
    uint16_t nextOffset = UINT16_MAX;
    for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) {
      const uint16_t size = glyphCacheEntrySize(glyphCacheEntries[i]);
      const uint16_t offset = glyphCacheEntries[i].bufferOffset;
      if (size > 0 && offset >= readCursor && offset < nextOffset) {
        next = i;
        nextOffset = offset;
      }
    }
    if (next < 0) break;

    GlyphCacheEntry& entry = glyphCacheEntries[next];
    const uint16_t size = glyphCacheEntrySize(entry);
    const uint16_t oldOffset = entry.bufferOffset;
    if (oldOffset != writeOffset) {
      memmove(&glyphCacheBuffer[writeOffset], &glyphCacheBuffer[oldOffset], size);
    }
    entry.bufferOffset = writeOffset;
    readCursor = oldOffset + size;
    writeOffset += size;
  }

  for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) {
    if (glyphCacheEntrySize(glyphCacheEntries[i]) == 0) glyphCacheEntries[i].bufferOffset = 0;
  }
  glyphCacheUsedBytes = writeOffset;
}

int8_t FontDecompressor::acquireGlyphCacheFont(const EpdFontData* fontData) {
  if (const int8_t existing = findGlyphCacheFont(fontData); existing >= 0) return existing;

  for (uint8_t i = 0; i < MAX_GLYPH_CACHE_FONTS; ++i) {
    if (!glyphCacheFonts[i].fontData) {
      glyphCacheFonts[i] = {fontData, glyphCacheGeneration};
      return static_cast<int8_t>(i);
    }
  }

  int8_t victim = -1;
  uint16_t greatestAge = 0;
  for (uint8_t fontSlot = 0; fontSlot < MAX_GLYPH_CACHE_FONTS; ++fontSlot) {
    bool pinned = false;
    for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) {
      if ((glyphCacheEntries[i].key >> 16) == fontSlot && glyphCacheEntries[i].lastUsed == glyphCacheGeneration) {
        pinned = true;
        break;
      }
    }
    if (pinned) continue;

    const uint16_t age = glyphCacheGeneration - glyphCacheFonts[fontSlot].lastUsed;
    if (victim < 0 || age > greatestAge) {
      victim = static_cast<int8_t>(fontSlot);
      greatestAge = age;
    }
  }
  if (victim < 0) return -1;

  for (uint16_t i = glyphCacheEntryCount; i > 0; --i) {
    if ((glyphCacheEntries[i - 1].key >> 16) == static_cast<uint8_t>(victim)) eraseGlyphCacheEntry(i - 1);
  }
  compactGlyphCache();
  glyphCacheFonts[victim] = {fontData, glyphCacheGeneration};
  return victim;
}

int FontDecompressor::findGlyphCacheEntry(const uint32_t key) const {
  int left = 0;
  int right = glyphCacheEntryCount - 1;
  while (left <= right) {
    const int middle = left + (right - left) / 2;
    if (glyphCacheEntries[middle].key == key) return middle;
    if (glyphCacheEntries[middle].key < key)
      left = middle + 1;
    else
      right = middle - 1;
  }
  return -1;
}

bool FontDecompressor::ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed) {
  if (capacity >= needed) return true;
  // Grow-only, free-then-malloc: every caller fully rewrites the buffer after a grow, so the
  // old contents are dead -- freeing first gives the allocator its best shot on a tight heap.
  free(buf);
  buf = static_cast<uint8_t*>(malloc(needed));  // owned by FontDecompressor, freed in freeHotGroup()
  capacity = buf ? needed : 0;
  return buf != nullptr;
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

  const bool isGlyphStream = fontData->bitmapFormat == EPD_BITMAP_FORMAT_GLYPH_STREAM_V1;
  if (!isGlyphStream && (!fontData->groups || fontData->groupCount == 0)) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  if (isGlyphStream) {
    const int8_t fontSlot = findGlyphCacheFont(fontData);
    if (fontSlot >= 0 && glyphIndex <= UINT16_MAX) {
      const uint32_t key = (static_cast<uint32_t>(fontSlot) << 16) | glyphIndex;
      const int entryIndex = findGlyphCacheEntry(key);
      if (entryIndex >= 0) {
        glyphCacheEntries[entryIndex].lastUsed = glyphCacheGeneration;
        glyphCacheFonts[fontSlot].lastUsed = glyphCacheGeneration;
        stats.cacheHits++;
        stats.getBitmapTimeUs += micros() - tStart;
        return &glyphCacheBuffer[glyphCacheEntries[entryIndex].bufferOffset];
      }
    }
  }

  if (isGlyphStream) {
    const size_t bitmapSize = GlyphStreamCodec::packedSize(glyph->width, glyph->height, fontData->is2Bit);
    if (!ensureCapacity(hotGlyphBuf, hotGlyphBufCapacity, bitmapSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for GlyphStream output", static_cast<unsigned>(bitmapSize));
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }
    stats.cacheMisses++;
    if (!GlyphStreamCodec::decode(fontData, glyphIndex, true, glyphStreamScratchA.data(), glyphStreamScratchB.data(),
                                  hotGlyphBuf, bitmapSize)) {
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }
    stats.getBitmapTimeUs += micros() - tStart;
    return hotGlyphBuf;
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(hotGroup != nullptr && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    // ensureCapacity may free the buffer, so the cached-group identity dies with it either way.
    hotGroupFont = nullptr;
    hotGroupIndex = UINT16_MAX;
    if (!ensureCapacity(hotGroup, hotGroupCapacity, group.uncompressedSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup, group.uncompressedSize)) {
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (!ensureCapacity(hotGlyphBuf, hotGlyphBufCapacity, glyph->dataLength)) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for glyph scratch", (unsigned)glyph->dataLength);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf, glyph->width, glyph->height);
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

bool FontDecompressor::prewarmGlyphStreamLru(const EpdFontData* fontData, const uint32_t* neededGlyphs,
                                             const uint16_t glyphCount, int& missed) {
  missed = 0;
  if (glyphCacheGeneration == 0) beginPrewarm();
  if (!ensureGlyphCache()) return false;

  uint32_t requestedBytes = 0;
  for (uint16_t i = 0; i < glyphCount; ++i) {
    if (neededGlyphs[i] > UINT16_MAX) return false;
    const EpdGlyph& glyph = fontData->glyph[neededGlyphs[i]];
    requestedBytes += GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit);
  }
  if (glyphCount > MAX_GLYPH_CACHE_ENTRIES || requestedBytes > GLYPH_CACHE_DATA_BYTES) return false;

  const int8_t fontSlot = acquireGlyphCacheFont(fontData);
  if (fontSlot < 0) return false;

  uint16_t pinnedEntries = 0;
  uint32_t pinnedBytes = 0;
  for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) {
    if (glyphCacheEntries[i].lastUsed == glyphCacheGeneration) {
      pinnedEntries++;
      pinnedBytes += glyphCacheEntrySize(glyphCacheEntries[i]);
    }
  }

  uint16_t newlyPinnedEntries = 0;
  uint32_t newlyPinnedBytes = 0;
  uint16_t missingEntries = 0;
  uint32_t missingBytes = 0;
  for (uint16_t i = 0; i < glyphCount; ++i) {
    const uint32_t key = (static_cast<uint32_t>(fontSlot) << 16) | neededGlyphs[i];
    const int entryIndex = findGlyphCacheEntry(key);
    const EpdGlyph& glyph = fontData->glyph[neededGlyphs[i]];
    const uint16_t size =
        static_cast<uint16_t>(GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit));
    if (entryIndex >= 0) {
      if (glyphCacheEntries[entryIndex].lastUsed != glyphCacheGeneration) {
        newlyPinnedEntries++;
        newlyPinnedBytes += size;
      }
    } else {
      missingEntries++;
      missingBytes += size;
    }
  }

  if (pinnedEntries + newlyPinnedEntries + missingEntries > MAX_GLYPH_CACHE_ENTRIES ||
      pinnedBytes + newlyPinnedBytes + missingBytes > GLYPH_CACHE_DATA_BYTES) {
    return false;
  }

  for (uint16_t i = 0; i < glyphCount; ++i) {
    const uint32_t key = (static_cast<uint32_t>(fontSlot) << 16) | neededGlyphs[i];
    const int entryIndex = findGlyphCacheEntry(key);
    if (entryIndex < 0) continue;

    glyphCacheEntries[entryIndex].lastUsed = glyphCacheGeneration;
    const EpdGlyph& glyph = fontData->glyph[neededGlyphs[i]];
    stats.prewarmGlyphCacheHits++;
    stats.prewarmGlyphCacheHitPixels += static_cast<uint32_t>(glyph.width) * glyph.height;
  }

  bool evicted = false;
  while (glyphCacheEntryCount + missingEntries > MAX_GLYPH_CACHE_ENTRIES ||
         glyphCacheUsedBytes + missingBytes > GLYPH_CACHE_DATA_BYTES) {
    int victim = -1;
    uint16_t greatestAge = 0;
    for (uint16_t i = 0; i < glyphCacheEntryCount; ++i) {
      if (glyphCacheEntries[i].lastUsed == glyphCacheGeneration) continue;
      const uint16_t age = glyphCacheGeneration - glyphCacheEntries[i].lastUsed;
      if (victim < 0 || age > greatestAge) {
        victim = i;
        greatestAge = age;
      }
    }
    if (victim < 0) return false;
    eraseGlyphCacheEntry(static_cast<uint16_t>(victim));
    evicted = true;
  }
  if (evicted) compactGlyphCache();

  for (uint16_t i = 0; i < glyphCount; ++i) {
    const uint32_t key = (static_cast<uint32_t>(fontSlot) << 16) | neededGlyphs[i];
    if (findGlyphCacheEntry(key) >= 0) continue;

    const EpdGlyph& glyph = fontData->glyph[neededGlyphs[i]];
    const uint16_t bitmapSize =
        static_cast<uint16_t>(GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit));
#if defined(GLYPH_STREAM_PERF_METRICS)
    uint32_t decodedPixels = 0;
#endif
    stats.prewarmGlyphCacheMisses++;
    if (!GlyphStreamCodec::decode(fontData, neededGlyphs[i], true, glyphStreamScratchA.data(),
                                  glyphStreamScratchB.data(), &glyphCacheBuffer[glyphCacheUsedBytes], bitmapSize
#if defined(GLYPH_STREAM_PERF_METRICS)
                                  ,
                                  &decodedPixels
#endif
                                  )) {
      missed++;
      continue;
    }
#if defined(GLYPH_STREAM_PERF_METRICS)
    stats.prewarmGlyphStreamDecodedPixels += decodedPixels;
#endif

    uint16_t insertAt = glyphCacheEntryCount;
    while (insertAt > 0 && glyphCacheEntries[insertAt - 1].key > key) {
      glyphCacheEntries[insertAt] = glyphCacheEntries[insertAt - 1];
      insertAt--;
    }
    glyphCacheEntries[insertAt] = {key, glyphCacheUsedBytes, glyphCacheGeneration};
    glyphCacheEntryCount++;
    glyphCacheUsedBytes += bitmapSize;
  }

  glyphCacheFonts[fontSlot].lastUsed = glyphCacheGeneration;
  stats.glyphCacheBytes = glyphCacheUsedBytes;
  stats.glyphCacheEntries = glyphCacheEntryCount;
  LOG_DBG("FDC", "GlyphStream LRU: %u requested, %u hits, %u misses, %u bytes resident", glyphCount,
          stats.prewarmGlyphCacheHits, stats.prewarmGlyphCacheMisses, glyphCacheUsedBytes);
  return true;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  if (!fontData || !utf8Text) return 0;
  const bool isGlyphStream = fontData->bitmapFormat == EPD_BITMAP_FORMAT_GLYPH_STREAM_V1;
  if (!isGlyphStream && !fontData->groups) return 0;

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

  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      // Check if both inputs are in neededGlyphs
      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      // Deduplicate
      bool found = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(outIdx)) {
          found = true;
          break;
        }
      }
      if (!found) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(outIdx);
      }
    }
  }

  if (glyphCount == 0) return 0;
  stats.prewarmGlyphs += glyphCount;

  for (uint16_t i = 1; i < glyphCount; ++i) {
    const uint32_t key = neededGlyphs[i];
    int j = i - 1;
    while (j >= 0 && neededGlyphs[j] > key) {
      neededGlyphs[j + 1] = neededGlyphs[j];
      j--;
    }
    neededGlyphs[j + 1] = key;
  }

  if (isGlyphStream) {
    for (uint16_t i = 0; i < glyphCount; ++i) {
      const EpdGlyph& glyph = fontData->glyph[neededGlyphs[i]];
      stats.prewarmGlyphStreamPixels += static_cast<uint32_t>(glyph.width) * glyph.height;
    }
    int lruMissed = 0;
    if (prewarmGlyphStreamLru(fontData, neededGlyphs, glyphCount, lruMissed)) return lruMissed;
  }

  // Oversized GlyphStream pages and legacy DEFLATE fonts use transient page slots.
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
    return -1;
  }
  PageSlot& slot = pageSlots[pageSlotCount];

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    const EpdGlyph& glyph = fontData->glyph[neededGlyphs[i]];
    totalBytes +=
        isGlyphStream ? GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit) : glyph.dataLength;
    if (isGlyphStream) continue;
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

  // Step 3: Allocate page buffer and lookup table for this slot
  slot.buffer = static_cast<uint8_t*>(malloc(totalBytes > 0 ? totalBytes : 1));
  slot.glyphs = static_cast<PageGlyphEntry*>(malloc(glyphCount * sizeof(PageGlyphEntry)));
  if (!slot.buffer || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    free(slot.buffer);
    free(slot.glyphs);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  // Sort by glyphIndex for binary search in getBitmap()
  for (uint16_t i = 1; i < glyphCount; i++) {
    PageGlyphEntry key = slot.glyphs[i];
    int j = i - 1;
    while (j >= 0 && slot.glyphs[j].glyphIndex > key.glyphIndex) {
      slot.glyphs[j + 1] = slot.glyphs[j];
      j--;
    }
    slot.glyphs[j + 1] = key;
  }

  if (isGlyphStream) {
    uint32_t writeOffset = 0;
    int missed = 0;
    stats.prewarmGlyphCacheMisses += glyphCount;
    for (uint16_t i = 0; i < slot.glyphCount; ++i) {
      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
#if defined(GLYPH_STREAM_PERF_METRICS)
      uint32_t decodedPixels = 0;
#endif
      const size_t bitmapSize = GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit);
      if (!GlyphStreamCodec::decode(fontData, slot.glyphs[i].glyphIndex, true, glyphStreamScratchA.data(),
                                    glyphStreamScratchB.data(), &slot.buffer[writeOffset], bitmapSize
#if defined(GLYPH_STREAM_PERF_METRICS)
                                    ,
                                    &decodedPixels
#endif
                                    )) {
        missed++;
        continue;
      }
#if defined(GLYPH_STREAM_PERF_METRICS)
      stats.prewarmGlyphStreamDecodedPixels += decodedPixels;
#endif
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += bitmapSize;
    }
    LOG_DBG("FDC", "Prewarm: %u GlyphStream glyphs in %u bytes (%d missed)", glyphCount, writeOffset, missed);
    return missed;
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
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = (int)slot.glyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
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

        int left = 0, right = (int)slot.glyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
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
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height);
      slot.glyphs[i].bufferOffset = writeOffset;
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
  LOG_DBG("FDC", "[%s] prewarm: glyphs=%u glyphstream_pixels=%lu", label, stats.prewarmGlyphs,
          stats.prewarmGlyphStreamPixels);
  LOG_DBG("FDC", "[%s] glyph_lru: hits=%u misses=%u hit_pixels=%lu entries=%u bytes=%lu", label,
          stats.prewarmGlyphCacheHits, stats.prewarmGlyphCacheMisses, stats.prewarmGlyphCacheHitPixels,
          stats.glyphCacheEntries, stats.glyphCacheBytes);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
