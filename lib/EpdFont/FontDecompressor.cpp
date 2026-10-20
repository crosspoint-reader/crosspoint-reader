#include "FontDecompressor.h"

#include <Arduino.h>
#include <BuildArena.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "GlyphProvider.h"  // glyph_miss::arena()

namespace {

// One group's inflate buffer, for the duration of one decompress-and-extract.
//
// Prefers the slot arena when one is installed, because the moment this allocation fails is
// precisely the moment the arena exists and is half empty: a page drawn *during* a section build.
// Measured X4 2026-08-18, mid-build draw with the framebuffer lent to the build — four
// consecutive refusals (7796, 8110, 8184, 8145 bytes) against contig 14324, while the borrowed
// 48000-byte region the slots were already drawing from had room for all of them. The slots moved
// there for the same reason (see FontCacheManager::ScopedSlotArena); the group temp was simply
// left behind on the heap.
//
// Falls back to the heap when there is no arena or when the build has the arena filled, so this
// can only ever turn a failure into a success.
class GroupTemp {
 public:
  GroupTemp(BuildArena* arena, const uint32_t bytes) {
    if (arena && arena->valid()) {
      // Nested inside the page-wide block ScopedSlotArena holds. Released before this object
      // dies, so the newest-first ordering release() enforces always holds.
      block_ = arena->reserveBlock();
      if (block_.valid()) {
        buf_ = static_cast<uint8_t*>(arena->alloc(bytes));
        if (buf_) {
          arena_ = arena;
          return;
        }
        arena->release(block_);
      }
    }
    buf_ = static_cast<uint8_t*>(malloc(bytes));
  }
  ~GroupTemp() {
    if (arena_) {
      arena_->release(block_);
    } else {
      free(buf_);
    }
  }
  GroupTemp(const GroupTemp&) = delete;
  GroupTemp& operator=(const GroupTemp&) = delete;

  uint8_t* get() const { return buf_; }
  bool fromArena() const { return arena_ != nullptr; }

 private:
  BuildArena* arena_ = nullptr;
  BuildArena::Block block_;
  uint8_t* buf_ = nullptr;
};

// Where a group's ring is taken first: the slot arena, else the region lent to the glyph misses
// (glyph_miss::Scope). A page render lends the secondary framebuffer there for its prewarm, and a
// ring is given back before its group is done, so nothing of it outlives the lend; the slots never
// come from that region, so they last into the render and its AA pass.
BuildArena* ringArena(BuildArena* slotArena) { return slotArena ? slotArena : glyph_miss::arena(); }

// Room left in an installed arena, for the OOM diagnostics: distinguishes "no arena here" from
// "arena was there and full", which are different bugs with the same symptom.
uint32_t arenaHeadroom(const BuildArena* arena) {
  if (!arena || !arena->valid()) return 0;
  return static_cast<uint32_t>(arena->capacity() - arena->used());
}

}  // namespace

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() { freePageBuffer(); }

void FontDecompressor::clearCache() { freePageBuffer(); }

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    // Arena-backed slots are not ours to free: the region belongs to whoever installed it (see
    // setSlotArena) and is rewound wholesale by its owner. free() on an interior pointer into
    // that region would corrupt the heap.
    if (!pageSlots[s].arenaBacked) {
      free(pageSlots[s].buffer);
      free(pageSlots[s].glyphs);
    }
    pageSlots[s] = {};
  }
  pageSlotCount = 0;
}

bool FontDecompressor::hasArenaBackedSlots() const {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    if (pageSlots[s].arenaBacked) return true;
  }
  return false;
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

// --- Streaming group reads ---

static_assert(sizeof(EpdFontGroup) == 20, "ringBytes must fit the padding hole, not grow the table");

uint32_t FontDecompressor::ringBytesFor(const EpdFontGroup& group) {
  const uint32_t measured = group.ringBytes != 0 ? group.ringBytes : group.uncompressedSize;
  // A back-reference can never reach further back than the bytes produced so far, so the group
  // is always a sufficient ring — and for a group smaller than its own window it is the cheaper
  // of the two. Also the safety net for a font whose ringBytes is somehow too large.
  return std::min(measured, group.uncompressedSize);
}

uint32_t FontDecompressor::maxRingBytes(const EpdFontData* face) {
  uint32_t most = 0;
  if (!face || !face->groups) return 0;
  for (uint16_t g = 0; g < face->groupCount; g++) most = std::max(most, ringBytesFor(face->groups[g]));
  return most;
}

bool FontDecompressor::inflateFace(const EpdFontData* face, uint8_t* ring, const uint32_t ringBytes, uint8_t* glyphBuf,
                                   bool (*sink)(void* ctx, const uint8_t* bytes, uint32_t len), void* ctx) {
  if (!face || !face->groups || face->groupCount == 0 || face->glyphToGroup || !ring || !glyphBuf || !sink) {
    return false;
  }
  // Contiguous groups tile the glyph array in order, so walking them walks every glyph once, in the
  // order the flat bitmap holds them.
  uint32_t next = 0;
  for (uint16_t g = 0; g < face->groupCount; g++) {
    const EpdFontGroup& group = face->groups[g];
    const uint32_t need = ringBytesFor(group);
    if (group.firstGlyphIndex != next || need > ringBytes) return false;
    GroupStream stream;
    if (!stream.begin(face, group, ring, need)) return false;
    uint32_t aligned = 0;
    for (uint32_t i = group.firstGlyphIndex; i < group.firstGlyphIndex + group.glyphCount; i++) {
      const EpdGlyphRef glyph = epdResolveGlyph(face, i);
      const uint16_t bytes = glyphDataBytes(glyph.width, glyph.height, face->is2Bit);
      if (bytes == 0) continue;  // no pixels: nothing in the stream, nothing in the flat bitmap
      if (bytes > HOT_GLYPH_BUF_SIZE || !stream.extractGlyph(aligned, glyph, glyphBuf)) return false;
      if (!sink(ctx, glyphBuf, bytes)) return false;
      aligned += groupRowBytes(glyph.width, face->is2Bit) * glyph.height;
    }
    next = group.firstGlyphIndex + group.glyphCount;
  }
  return true;
}

bool FontDecompressor::GroupStream::begin(const EpdFontData* fontData, const EpdFontGroup& group, uint8_t* ring,
                                          const uint32_t ringBytes) {
  if (!ring || ringBytes == 0) return false;
  if (!reader_.initWithExternalRing(ring, ringBytes)) return false;
  reader_.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  pos_ = 0;
  limit_ = group.uncompressedSize;
  is2Bit_ = fontData->is2Bit;
  return true;
}

bool FontDecompressor::GroupStream::readExact(uint8_t* dest, uint32_t len) {
  while (len > 0) {
    size_t produced = 0;
    const InflateStatus st = reader_.readAtMost(dest, len, &produced);
    if (st == InflateStatus::Error) return false;
    // A clean end that produced nothing means the stream is shorter than the group table says,
    // or a back-reference outran the ring (uzlib reports TINF_DICT_ERROR for that, so a font
    // whose ringBytes is too small fails here loudly instead of decoding to garbage).
    if (produced == 0) return false;
    dest += produced;
    len -= static_cast<uint32_t>(produced);
    pos_ += static_cast<uint32_t>(produced);
  }
  return true;
}

bool FontDecompressor::GroupStream::skipTo(const uint32_t offset) {
  if (offset < pos_) return false;  // a stream cannot rewind; see the ordering contract
  uint8_t sink[MAX_ROW_STRIDE];
  while (pos_ < offset) {
    const uint32_t want = std::min<uint32_t>(sizeof(sink), offset - pos_);
    if (!readExact(sink, want)) return false;
  }
  return true;
}

bool FontDecompressor::GroupStream::extractGlyph(const uint32_t alignedOffset, const EpdGlyphRef& glyph,
                                                 uint8_t* packedDst) {
  if (glyph.width == 0 || glyph.height == 0) return true;  // no payload; consumes nothing
  const uint32_t rowStride = groupRowBytes(glyph.width, is2Bit_);
  if (rowStride > MAX_ROW_STRIDE) return false;  // unreachable for a uint8_t width; guards the buffers
  if (alignedOffset + rowStride * glyph.height > limit_) return false;
  if (!skipTo(alignedOffset)) return false;

  // A width that fills its last row byte exactly leaves no row padding, so the byte-aligned and
  // packed forms are identical and the stream lands straight in the destination.
  if (glyph.width % (is2Bit_ ? 4 : 8) == 0) return readExact(packedDst, rowStride * glyph.height);

  uint8_t row[MAX_ROW_STRIDE];
  uint8_t outByte = 0;
  uint8_t outBits = 0;
  uint32_t writeIdx = 0;
  if (!is2Bit_) {
    // The 1-bit UI faces: the same repack one pixel per bit. Inverse of row_aligned() in
    // compress_ui_face.py.
    for (uint8_t y = 0; y < glyph.height; y++) {
      if (!readExact(row, rowStride)) return false;
      for (uint8_t x = 0; x < glyph.width; x++) {
        outByte = static_cast<uint8_t>((outByte << 1) | ((row[x / 8] >> (7 - (x % 8))) & 0x1));
        if (++outBits == 8) {
          packedDst[writeIdx++] = outByte;
          outByte = 0;
          outBits = 0;
        }
      }
    }
    if (outBits > 0) packedDst[writeIdx] = static_cast<uint8_t>(outByte << (8 - outBits));
    return true;
  }

  // Otherwise repack as the rows arrive. The bit accumulator deliberately carries across rows:
  // packed pixels flow continuously where byte-aligned rows are padded to a byte. Same bit order
  // as to_byte_aligned() in fontconvert.py, read backwards.
  for (uint8_t y = 0; y < glyph.height; y++) {
    if (!readExact(row, rowStride)) return false;
    for (uint8_t x = 0; x < glyph.width; x++) {
      outByte = static_cast<uint8_t>((outByte << 2) | ((row[x / 4] >> ((3 - (x % 4)) * 2)) & 0x3));
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = static_cast<uint8_t>(outByte << (8 - outBits));
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyphRef& g) {
    if (g.width > 0 && g.height > 0) {
      offset += groupRowBytes(g.width, fontData->is2Bit) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(epdResolveGlyph(fontData, i));
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(epdResolveGlyph(fontData, i));
    }
  }

  return offset;
}

// --- getBitmap: page buffer → transient ring + streamed compact ---

// dataLength is derived rather than stored (see EpdGlyphPacked), and deliberately NOT carried on
// EpdGlyphRef: the measurement path resolves a glyph per character and would pay the multiply
// without ever reading the result. The bitmap paths, which are the only ones that want it, ask.
static inline uint16_t dataLengthOf(const EpdFontData* fontData, const EpdGlyphRef& g) {
  return glyphDataBytes(g.width, g.height, fontData->is2Bit);
}

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyphRef& glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[epdGlyphBitmapOffset(fontData, glyph)];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per distinct EpdFontData,
  // i.e. per style AND size)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          // Actual render use, not just prewarm order, is what should keep a slot alive.
          slot.lastUsedTick = ++pageSlotTick_;
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

  // Check fallback LRU cache
  for (uint8_t i = 0; i < FALLBACK_CACHE_SLOTS; i++) {
    if (_fallbackCache[i].fontData == fontData && _fallbackCache[i].glyphIndex == glyphIndex) {
      _fallbackCache[i].lastUsedTick = ++_fallbackTick;
      stats.cacheHits++;
      stats.fallbackCacheHits++;
      stats.getBitmapTimeUs += micros() - tStart;
      return _fallbackCache[i].buffer;
    }
  }

  stats.fallbackCacheMisses++;

  // Fallback: glyph wasn't in the page buffer — decompress its group transiently.
  // This is the rare path (prewarm should cover all glyphs on a normal page).
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  stats.cacheMisses++;
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint16_t glyphBytes = dataLengthOf(fontData, glyph);
  if (glyphBytes > HOT_GLYPH_BUF_SIZE) {
    LOG_ERR("FDC", "Glyph dataLength %u exceeds HOT_GLYPH_BUF_SIZE %u", glyphBytes, HOT_GLYPH_BUF_SIZE);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // What this glyph costs to reach is the group's ring, not the group (see ringBytesFor).
  const uint32_t ringBytes = ringBytesFor(group);
  if (ringBytes > stats.peakTempBytes) stats.peakTempBytes = ringBytes;

  // OOM latch. This is the per-glyph-occurrence path, so a page whose prewarm did not cover
  // it calls through here hundreds of times. The heap cannot recover mid-pass (the render
  // lock is held throughout), so once an allocation of a given size has failed, every later
  // request at or above that size fails too — retrying is pure waste. Measured on X4
  // (2026-08-02, a render racing a background section build): 538 failed mallocs and 538
  // identical ERR lines in 40 ms, 147 ms of getBitmap time against ~1 ms normally, and a
  // page render of 129 ms against 43. Only the failing size class is latched out; a smaller
  // group may still fit and is still attempted.
  //
  // Reader (2-bit) faces only. The pass the latch lasts for ends in logStats(), which only the
  // reader calls: a UI screen never does, so a latched 1-bit UI face would stay blank for the
  // rest of the session. A UI face decodes here only for the few CJK glyphs a screen draws from a
  // face the language pack has not materialised (CjkUiFonts), so it simply retries each time.
  const bool latches = fontData->is2Bit;
  if (latches && stats.fallbackOomBytes != 0 && ringBytes >= stats.fallbackOomBytes) {
    stats.fallbackOomGlyphs++;
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  GroupTemp temp(ringArena(slotArena_), ringBytes);
  uint8_t* ring = temp.get();
  if (!ring) {
    // Logged once per size class per pass for a reader face, once per glyph for a UI face (see
    // above); the running total goes out in logStats().
    LOG_ERR("FDC", "OOM: cannot allocate a %lu-byte ring for group %u fallback (arena headroom %lu)%s", ringBytes,
            groupIndex, arenaHeadroom(ringArena(slotArena_)),
            latches ? "; skipping this size and larger for the pass" : "");
    if (latches) stats.fallbackOomBytes = ringBytes;
    stats.fallbackOomGlyphs++;
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }
  if (temp.fromArena()) stats.arenaTemps++;

  const uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);

  uint8_t lruIndex = 0;
  uint32_t oldestTick = UINT32_MAX;
  for (uint8_t i = 0; i < FALLBACK_CACHE_SLOTS; i++) {
    if (_fallbackCache[i].lastUsedTick < oldestTick) {
      oldestTick = _fallbackCache[i].lastUsedTick;
      lruIndex = i;
    }
  }

  // Streams only as far as this one glyph and stops — the tail of the group is never decoded,
  // which is where the fallback path wins back the CPU that a whole-group inflate spent.
  const uint32_t tDecomp = millis();
  GroupStream stream;
  if (!stream.begin(fontData, group, ring, ringBytes) ||
      !stream.extractGlyph(alignedOff, glyph, _fallbackCache[lruIndex].buffer)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Streaming group %u failed at glyph %lu", groupIndex, glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  stats.streamedBytes += stream.consumed();
  stats.groupBytes += group.uncompressedSize;

  _fallbackCache[lruIndex].fontData = fontData;
  _fallbackCache[lruIndex].glyphIndex = glyphIndex;
  _fallbackCache[lruIndex].lastUsedTick = ++_fallbackTick;

  stats.getBitmapTimeUs += micros() - tStart;
  return _fallbackCache[lruIndex].buffer;
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

    const EpdGlyphRef glyph = epdResolveGlyph(fontData, glyphIdx);
    // Whitespace/empty glyphs have no bitmap payload and do not need prewarm storage.
    if (glyph.width == 0 || glyph.height == 0) continue;  // no payload; dataLength would be 0 too

    // Deduplicate against already prewarmed slots
    bool alreadyCached = false;
    for (uint8_t s = 0; s < pageSlotCount; s++) {
      if (pageSlots[s].fontData != fontData || pageSlots[s].glyphCount == 0) continue;
      int left = 0, right = pageSlots[s].glyphCount - 1;
      while (left <= right) {
        int mid = left + (right - left) / 2;
        if (pageSlots[s].glyphs[mid].glyphIndex == static_cast<uint32_t>(glyphIdx)) {
          if (pageSlots[s].glyphs[mid].bufferOffset != UINT32_MAX) {
            alreadyCached = true;
          }
          break;
        }
        if (pageSlots[s].glyphs[mid].glyphIndex < static_cast<uint32_t>(glyphIdx))
          left = mid + 1;
        else
          right = mid - 1;
      }
      if (alreadyCached) break;
    }
    if (alreadyCached) continue;

    // Deduplicate within the current page pass
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
  // Must run BEFORE the neededGlyphGroups[] parallel-array loop below so appended
  // glyphs receive a group index — otherwise hot-group lookup misses them.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;
      const EpdGlyphRef outGlyph = epdResolveGlyph(fontData, outIdx);
      if (outGlyph.width == 0 || outGlyph.height == 0) continue;  // no payload

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

  // Allocate the next available slot, evicting the least-recently-used one when full.
  // Refusing instead (the old behaviour) sent every glyph of the refused font down the
  // per-glyph fallback for the rest of the page — measured at 7562 us per glyph.
  uint8_t slotIndex = pageSlotCount;
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    slotIndex = 0;
    uint32_t oldestTick = UINT32_MAX;
    for (uint8_t s = 0; s < MAX_PAGE_SLOTS; s++) {
      if (pageSlots[s].lastUsedTick < oldestTick) {
        oldestTick = pageSlots[s].lastUsedTick;
        slotIndex = s;
      }
    }
    LOG_DBG("FDC", "Page slots full; evicting LRU slot %u (fontData=%p) for fontData=%p", slotIndex,
            (void*)pageSlots[slotIndex].fontData, (void*)fontData);
    // Callers must not hold a getBitmap() pointer across this — the documented contract is that
    // a returned bitmap is valid only until the next getBitmap call or a cache eviction.
    // Clamped: resetStats() can zero these between the prewarm that added them and this
    // eviction, and an unsigned underflow would report a nonsense footprint.
    const uint32_t evictedGlyphBytes = pageSlots[slotIndex].glyphCount * sizeof(PageGlyphEntry);
    stats.pageBufferBytes -= std::min(stats.pageBufferBytes, pageSlots[slotIndex].bufferBytes);
    stats.pageGlyphsBytes -= std::min(stats.pageGlyphsBytes, evictedGlyphBytes);
    if (!pageSlots[slotIndex].arenaBacked) {
      free(pageSlots[slotIndex].buffer);
      free(pageSlots[slotIndex].glyphs);
    }
    // An evicted arena slot leaks its bytes for the rest of the arena scope: the arena bump-
    // allocates and this block is no longer the newest. Bounded and short-lived (one page draw,
    // MAX_PAGE_SLOTS is 4), so it is cheaper than tracking a block token per slot.
    pageSlots[slotIndex] = {};
    pageSlotCount--;
    // Keep the live slots contiguous in [0, pageSlotCount): the scan loops rely on it.
    for (uint8_t s = slotIndex; s < pageSlotCount; s++) {
      pageSlots[s] = pageSlots[s + 1];
    }
    pageSlots[pageSlotCount] = {};
    slotIndex = pageSlotCount;
  }
  PageSlot& slot = pageSlots[slotIndex];

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint16_t neededGlyphGroups[MAX_PAGE_GLYPHS];  // parallel to neededGlyphs; avoids re-calling getGroupIndex later
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += dataLengthOf(fontData, epdResolveGlyph(fontData, neededGlyphs[i]));
    const uint16_t gi = getGroupIndex(fontData, neededGlyphs[i]);
    neededGlyphGroups[i] = gi;
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

  // Safety: if the collected glyph set has no bitmap payload, skip slot allocation.
  if (totalBytes == 0) {
    LOG_DBG("FDC", "Prewarm skipped: %u glyphs but 0 bitmap bytes", glyphCount);
    return 0;
  }

  // Sort neededGroups by ascending group index so flash reads are sequential.
  // Uses insertion sort — groupCount is bounded at 128, typically <14 for Latin fonts.
  for (uint8_t i = 1; i < groupCount; i++) {
    uint16_t key = neededGroups[i];
    int j = i - 1;
    while (j >= 0 && neededGroups[j] > key) {
      neededGroups[j + 1] = neededGroups[j];
      j--;
    }
    neededGroups[j + 1] = key;
  }

  // Step 3: Allocate page buffer and lookup table for this slot.
  //
  // From the scratch arena when one is installed (see setSlotArena). Both blocks come from a
  // single reserved scope so a partial failure rewinds cleanly and the pair can never end up
  // half-arena, half-heap. Falling back to the heap on refusal is the pre-existing behaviour,
  // so an arena too small for the page simply costs the anti-fragmentation benefit.
  const uint32_t glyphTableBytes = glyphCount * sizeof(PageGlyphEntry);
  slot.arenaBacked = false;
  if (slotArena_ && slotArena_->valid()) {
    // Plain bump allocation, no per-slot block. The whole page's slots live inside ONE scope
    // reserved by FontCacheManager::ScopedSlotArena and rewound when it exits, which is what
    // keeps the arena cursor from creeping: committing a block per slot would advance it ~9 KB
    // per mid-build draw with no way back until the build ended, and a reader paging forward
    // during a build would exhaust the region in three draws.
    auto* buf = static_cast<uint8_t*>(slotArena_->alloc(totalBytes));
    auto* glyphs = static_cast<PageGlyphEntry*>(slotArena_->alloc(glyphTableBytes, alignof(PageGlyphEntry)));
    if (buf && glyphs) {
      slot.buffer = buf;
      slot.glyphs = glyphs;
      slot.arenaBacked = true;
    }
    // A partial fit (buf but no glyphs, or neither) just falls through to the heap below. The
    // bytes taken are reclaimed with the rest of the scope, so there is nothing to unwind.
  }
  if (!slot.arenaBacked) {
    slot.buffer = static_cast<uint8_t*>(malloc(totalBytes));
    slot.glyphs = static_cast<PageGlyphEntry*>(malloc(glyphTableBytes));
  }
  if (!slot.buffer || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    if (!slot.arenaBacked) {
      free(slot.buffer);
      free(slot.glyphs);
    }
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  slot.bufferBytes = totalBytes;
  slot.lastUsedTick = ++pageSlotTick_;
  pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0, neededGlyphGroups[i]};
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

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup.
    // Reverse map (fontGroupIdx → position in neededGroups) replaces the inner
    // linear scan, dropping this pass from O(totalGlyphs × groupCount) to O(totalGlyphs).
    uint8_t* groupIdToPos = static_cast<uint8_t*>(malloc(fontData->groupCount));
    if (!groupIdToPos) {
      LOG_ERR("FDC", "OOM: cannot allocate %u bytes for groupIdToPos map", fontData->groupCount);
      // Roll back this slot only (other slots from prior prewarmCache calls stay valid)
      stats.pageBufferBytes -= totalBytes;
      stats.pageGlyphsBytes -= glyphCount * sizeof(PageGlyphEntry);
      // An arena-backed slot is an interior pointer into the lent framebuffer: its bytes go
      // back with the slot scope, and free() on them would corrupt the heap -- on exactly the
      // low-heap mid-build draw where a 128-byte malloc can fail (memory audit 2026-09, F2c).
      if (!slot.arenaBacked) {
        free(slot.buffer);
        free(slot.glyphs);
      }
      slot = {};
      pageSlotCount--;
      return glyphCount;
    }
    memset(groupIdToPos, 0xFF, fontData->groupCount);
    for (uint8_t j = 0; j < groupCount; j++) groupIdToPos[neededGroups[j]] = j;

    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      const uint8_t gpPos = groupIdToPos[gi];
      if (gpPos == 0xFF) continue;  // not a needed group

      const EpdGlyphRef glyph = epdResolveGlyph(fontData, i);

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
        groupAlignedTracker[gpPos] += groupRowBytes(glyph.width, fontData->is2Bit) * glyph.height;
      }
    }

    free(groupIdToPos);
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyphRef glyph = epdResolveGlyph(fontData, glyphI);

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
          alignedOff += groupRowBytes(glyph.width, fontData->is2Bit) * glyph.height;
        }
      }
    }
  }

  // Step 4: For each unique group, take a transient ring (arena first, else heap — see
  // GroupTemp), stream the group through it compacting needed glyphs as they pass, release.
  // Groups are visited in sorted order, so only one ring is alive at a time — peak = page buffer
  // + largest single ring, and the ring is the group's measured back-reference reach rather than
  // its uncompressed size (see ringBytesFor).
  uint32_t writeOffset = 0;
  int missed = 0;
  uint8_t arenaTempCount = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];
    const uint32_t ringBytes = ringBytesFor(group);

    if (ringBytes > stats.peakTempBytes) stats.peakTempBytes = ringBytes;

    // Glyphs this group owes the page, so a refusal reports what the reader actually loses
    // rather than counting groups. Computed up front: the extraction loop below is what would
    // otherwise clear them, and on the failure paths it never runs.
    uint16_t owed = 0;
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].groupIndex == groupIdx && slot.glyphs[i].bufferOffset == UINT32_MAX) owed++;
    }

    GroupTemp temp(ringArena(slotArena_), ringBytes);
    uint8_t* ring = temp.get();
    if (!ring) {
      LOG_ERR("FDC",
              "OOM: cannot allocate a %lu-byte ring for group %u during prewarm (arena headroom %lu, %u glyphs lost)",
              ringBytes, groupIdx, arenaHeadroom(ringArena(slotArena_)), owed);
      missed += owed;
      continue;
    }
    if (temp.fromArena()) {
      stats.arenaTemps++;
      arenaTempCount++;
    }

    const uint32_t tDecomp = millis();
    GroupStream stream;
    if (!stream.begin(fontData, group, ring, ringBytes)) {
      stats.decompressTimeMs += millis() - tDecomp;
      LOG_ERR("FDC", "Cannot start streaming group %u", groupIdx);
      missed += owed;
      continue;
    }

    // Compact needed glyphs straight out of the passing stream. alignedOffset was pre-computed
    // in step 3b, and ascending glyph index means ascending alignedOffset within a group, which
    // is exactly the forward-only order the stream requires. The loop stops at the last glyph
    // this page wants, so the rest of the group is never decoded.
    uint16_t extracted = 0;
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (slot.glyphs[i].groupIndex != groupIdx) continue;

      const EpdGlyphRef glyph = epdResolveGlyph(fontData, slot.glyphs[i].glyphIndex);
      if (!stream.extractGlyph(slot.glyphs[i].alignedOffset, glyph, &slot.buffer[writeOffset])) {
        LOG_ERR("FDC", "Streaming group %u failed after %u of %u glyph(s)", groupIdx, extracted, owed);
        break;
      }
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += dataLengthOf(fontData, glyph);
      extracted++;
    }
    stats.decompressTimeMs += millis() - tDecomp;
    stats.streamedBytes += stream.consumed();
    stats.groupBytes += group.uncompressedSize;
    missed += owed - extracted;
  }

  LOG_TRC("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  // Positive evidence that the arena path did something — only a draw racing a section build can
  // produce it (see GroupTemp), and it is silent on every ordinary page.
  //
  // Logged here rather than in logStats() because logStats() is only ever called from
  // renderContents(); displayBuildPage() — the one function that draws mid-build and the only
  // one that installs the slot arena — never calls it, so a stats-time report could not fire for
  // the very case this counter exists to prove.
  if (arenaTempCount > 0) {
    LOG_DBG("FDC", "prewarm: %u/%u group inflate(s) from the slot arena", arenaTempCount, groupCount);
  }

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  // Suppress the block entirely when the decompressor was untouched this phase
  // (e.g. an SD-card font page — FontCacheManager early-returns before invoking us).
  if (total == 0 && stats.pageBufferBytes == 0 && stats.decompressTimeMs == 0 && stats.getBitmapCalls == 0) {
    resetStats();
    return;
  }
  // Routine per-pass counters. Every number here is either duplicated by the reader's one-line
  // "Page summary" (fontHits / fontMisses / fontHitPct / glyphCalls / glyphUs) or only meaningful
  // when something is wrong, and at 5-7 lines per pass and two passes per page they were the
  // single largest source of noise in a reading trace.
  LOG_TRC("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_TRC("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_TRC("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu peakRing=%lu arenaTemps=%u", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.peakTempBytes, stats.arenaTemps);
  if (stats.groupBytes > 0) {
    LOG_TRC("FDC", "[%s] streamed %lu of %lu group bytes (%.0f%%)", label, stats.streamedBytes, stats.groupBytes,
            100.0f * stats.streamedBytes / stats.groupBytes);
  }

  // getBitmap timing is the exception: a prewarm that missed its font sends every glyph down the
  // hot-group path at thousands of us each (measured 5642 us/glyph against 1 us prewarmed), and
  // that is a real, visible stall worth seeing without raising the log level. So report it at DBG
  // only when it is actually pathological, and let the healthy case go to TRC with the rest.
  if (stats.getBitmapCalls > 0) {
    const uint32_t avgUs = stats.getBitmapTimeUs / stats.getBitmapCalls;
    // ~100 us/glyph is an order of magnitude above a cache hit and far below a group inflate,
    // so it separates the two populations without needing tuning.
    constexpr uint32_t SLOW_GLYPH_US = 100;
    if (avgUs >= SLOW_GLYPH_US) {
      LOG_DBG("FDC", "[%s] getBitmap SLOW: %lu calls, %luus total, %luus/call avg (prewarm missed this font)", label,
              stats.getBitmapCalls, stats.getBitmapTimeUs, avgUs);
    } else {
      LOG_TRC("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
              stats.getBitmapTimeUs, avgUs);
    }
  }

  // Same rule: the fallback cache only matters when it is missing, which is the state that makes
  // the line above slow.
  const uint32_t lruTotal = stats.fallbackCacheHits + stats.fallbackCacheMisses;
  if (lruTotal > 0) {
    if (stats.fallbackCacheMisses > 0) {
      LOG_DBG("FDC", "[%s] LRU Fallback: hits=%lu misses=%lu (%.1f%%)", label, stats.fallbackCacheHits,
              stats.fallbackCacheMisses, 100.0f * stats.fallbackCacheHits / lruTotal);
    } else {
      LOG_TRC("FDC", "[%s] LRU Fallback: hits=%lu misses=0", label, stats.fallbackCacheHits);
    }
  }

  // Degraded render: these glyphs drew nothing. One line per pass instead of one per glyph.
  if (stats.fallbackOomGlyphs > 0) {
    LOG_ERR("FDC", "[%s] fallback OOM: %u glyphs dropped (groups >=%lu bytes unavailable)", label,
            stats.fallbackOomGlyphs, stats.fallbackOomBytes);
  }

  resetStats();
}
