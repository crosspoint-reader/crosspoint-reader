#include "GfxRenderer.h"

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GlyphProvider.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <SmallCaps.h>
#include <TextTruncation.h>
#include <TouchTransform.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cassert>
#include <cstring>

#include "FontCacheManager.h"

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyphRef& glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    // The index used to be recovered by subtracting the array base from the returned pointer,
    // which a packed array makes impossible; EpdGlyphRef carries it instead.
    //
    // The returned BITMAP still has the old lifetime: stable for the page on a page-buffer hit,
    // valid only until the next getBitmap() call on a hot-group hit, so callers must draw it
    // before asking for another.
    return fd->getBitmap(fontData, glyph, glyph.index);
  }
  // A glyph the font's provider loaded itself (SdCardFont's miss ring) keeps its bitmap there.
  // ownsBitmap() answers by the record's pointer identity, which is why resolution carries
  // sdRecord; it is null for built-in fonts. An owned glyph with no pixels (a space) yields
  // nullptr and must not fall through to the flat bitmap below.
  if (fontData->provider && glyph.sdRecord) {
    const uint8_t* owned = nullptr;
    if (fontData->provider->ownsBitmap(glyph.sdRecord, owned)) return owned;
  }
  return &fontData->bitmap[epdGlyphBitmapOffset(fontData, glyph)];
}

void GfxRenderer::ensureFontReady(int fontId, const char* utf8Text) const {
  SdCardFont* font = sdCardFontFor(fontId);  // native ID or scaled alias
  if (!font) {
    // A face whose glyphs come from a provider (TTF) renders the run's missing glyphs in one batch.
    // Layout passes no style, so the regular face it measures most with; built-in fonts do nothing.
    if (const EpdFontFamily* family = findFontFamily(fontId)) family->prefetch(utf8Text, 1u << EpdFontFamily::REGULAR);
    return;
  }
  // Metadata-only: loads glyph metrics (advanceX) without bitmap data.
  // Saves ~50-100 KB heap vs full prewarm — layout only needs advance widths.
  int missed = font->prewarm(utf8Text, 0x0F, /*metadataOnly=*/true,
                             /*loadKernLigatureData=*/true);
  if (missed > 0) {
    LOG_DBG("GFX", "ensureFontReady: %d glyph(s) not found", missed);
  }
}

void GfxRenderer::clearFontAccumulation() const {
  for (auto& [id, font] : sdCardFonts_) {
    font->clearAccumulation();
  }
}

void GfxRenderer::setFontCacheManager(FontCacheManager* m) {
  fontCacheManager_ = m;
  if (!m) return;
  m->setFontLookup(
      [](const void* ctx, const int fontId) { return static_cast<const GfxRenderer*>(ctx)->findFontFamily(fontId); },
      this);
}

bool GfxRenderer::releaseFontCaches() {
  if (!fontCacheManager_) return false;
  fontCacheManager_->clearCache();
  return true;
}

void GfxRenderer::dropFontMetadata() const {
  for (auto& [id, font] : sdCardFonts_) {
    font->unloadMetadata();
  }
}

bool GfxRenderer::restoreFontMetadata() const {
  bool ok = true;
  for (auto& [id, font] : sdCardFonts_) {
    if (!font->reloadMetadata()) {
      LOG_ERR("GFX", "Failed to reload metadata for font %d", id);
      ok = false;
    }
  }
  return ok;
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwSnapshotRowStart = 0;
  bwSnapshotRowEnd = 0;
  bwSnapshotSizeBytes = 0;
  bwBufferChunkSize = BW_BUFFER_CHUNK_SIZE;
  bwBufferChunks.assign((frameBufferSize + bwBufferChunkSize - 1) / bwBufferChunkSize, nullptr);
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
  invalidateScaledGlyphCache();
}

// Bits needed for a w x h 1-bit mask, plus one guard byte: bitmapExtract() reads
// two bytes when a chunk straddles a byte boundary, so the final chunk of the
// last row may touch one byte past the packed size.
static inline uint16_t scaledGlyphMaskBytes(const int w, const int h) {
  return static_cast<uint16_t>((static_cast<int>(w) * h + 7) / 8 + 1);
}

// Reinterpret a float's bits so cache keys compare exactly. Two words laid out
// with the same CSS scale produce the same float, so this is a reliable identity
// without the rounding risk of quantising the scale into a fixed-point key.
static inline uint32_t floatBits(const float f) {
  uint32_t bits;
  memcpy(&bits, &f, sizeof(bits));
  return bits;
}

// Allocate the scaled-glyph cache. Call this EARLY — at reader entry, not on first scaled glyph.
//
// It is ~4.9 KB in two blocks (80 entries + a 3584-byte mask arena), taken here at reader entry and
// given back at reader exit (releaseGlyphCaches), never released or re-taken in between: a
// reader-lifetime allocation in the strategy note's class A. Allocating it lazily
// meant "first use" decided where a permanent block landed, and first use is often a heading
// inside a mid-build page draw — so it was being carved out of the middle of the largest free
// region while a section build held the rest of the heap. Device-measured X3 2026-08-11: 5032
// bytes taken during a mid-build draw cost 5120 of contig, permanently.
//
// That is eliminated #8 of the heap handover ("a permanent block taken mid-session pins the
// largest free region") arrived at by accident, via "allocate on first use". Taking it at a
// stable point puts it next to the other permanent allocations instead.
//
// Idempotent, and failure is non-fatal: scaled text renders uncached, exactly as before.
void GfxRenderer::releaseGlyphCaches() const {
  if (fontCacheManager_) fontCacheManager_->clearCache();
  releaseScaledGlyphCache();
}

bool GfxRenderer::ensureScaledGlyphCache(const int bodyFontId) const {
  // Larger when the body text is a synthesised size, because then the arena holds a whole page's
  // glyphs at up to 26 pt rather than a few CSS-scaled words. See SCALED_GLYPH_ARENA_BYTES_SYNTH.
  const uint16_t wanted = (bodyFontId != 0 && fontBaseScales.count(bodyFontId) != 0) ? SCALED_GLYPH_ARENA_BYTES_SYNTH
                                                                                     : SCALED_GLYPH_ARENA_BYTES;
  if (scaledGlyphArena_) {
    if (bodyFontId == 0 || scaledGlyphArenaBytes_ == wanted) return true;
    // The last book was read at the other kind of size. Replaced here, at the same stable point
    // the first one was taken at; the entries go with it, as the masks they point at do.
    scaledGlyphArena_.reset();
    scaledGlyphEntries_.reset();
    scaledGlyphCount_ = 0;
    scaledGlyphUsed_ = 0;
  }
  if (scaledGlyphOom_) return false;  // already failed once; don't thrash the heap
  scaledGlyphArenaBytes_ = wanted;
  auto entries = makeUniqueNoThrow<ScaledGlyphEntry[]>(SCALED_GLYPH_MAX_ENTRIES);
  auto arena = makeUniqueNoThrow<uint8_t[]>(scaledGlyphArenaBytes_);
  if (!entries || !arena) {
    LOG_ERR("GFX", "OOM: scaled-glyph cache (%u + %u bytes); rendering scaled text uncached",
            static_cast<unsigned>(SCALED_GLYPH_MAX_ENTRIES * sizeof(ScaledGlyphEntry)),
            static_cast<unsigned>(scaledGlyphArenaBytes_));
    scaledGlyphOom_ = true;
    return false;
  }
  scaledGlyphEntries_ = std::move(entries);
  scaledGlyphArena_ = std::move(arena);
  scaledGlyphCount_ = 0;
  scaledGlyphUsed_ = 0;
  return true;
}

const uint8_t* GfxRenderer::findScaledGlyphMask(const void* fontData, const uint32_t cp, const float scale,
                                                const uint8_t sel, const int w, const int h) const {
  if (!scaledGlyphArena_) return nullptr;
  const uint32_t bits = floatBits(scale);
  for (uint8_t i = 0; i < scaledGlyphCount_; i++) {
    const ScaledGlyphEntry& e = scaledGlyphEntries_[i];
    if (e.fontData == fontData && e.cp == cp && e.scaleBits == bits && e.sel == sel && e.w == w && e.h == h) {
      return scaledGlyphArena_.get() + e.offset;
    }
  }
  return nullptr;
}

uint8_t* GfxRenderer::allocScaledGlyphMask(const void* fontData, const uint32_t cp, const float scale,
                                           const uint8_t sel, const int w, const int h) const {
  // Dimensions are stored as uint8 and the mask must stay well under the arena;
  // oversized glyphs render uncached rather than evicting the body-text set.
  // Codepoints above the BMP are stored in a uint16 key, so they render uncached
  // too — they are vanishingly rare in body text and never worth widening the key.
  if (w <= 0 || h <= 0 || w > 255 || h > 255 || cp > 0xFFFF) return nullptr;
  const uint16_t bytes = scaledGlyphMaskBytes(w, h);
  if (bytes > SCALED_GLYPH_MAX_MASK_BYTES) return nullptr;

  if (!scaledGlyphArena_ && !ensureScaledGlyphCache()) return nullptr;

  if (scaledGlyphCount_ >= SCALED_GLYPH_MAX_ENTRIES || scaledGlyphUsed_ + bytes > scaledGlyphArenaBytes_) {
    // Wholesale reset instead of LRU bookkeeping: the working set is one page's
    // distinct glyphs, so a reset costs at most one re-resample each.
    //
    // TRC, not DBG, and deliberately not device-gated: for CSS-scaled body text
    // this is a designed, cheap event (the X4 Pro hit the 80-entry cap at ~1.9 KB
    // used; page summaries measured ~1.5 us per glyph call, i.e. mostly hits on
    // ~40 B masks). That "beneath notice" verdict does NOT extend to a synthesised
    // body size: there every glyph is a resample of a 20 pt master glyph, ~1 ms
    // each at 24-26 pt, so a reset re-costs the page's working set at hundreds of
    // times the rate the measurement above saw. That is why the arena is larger
    // once a scaled font is registered -- see SCALED_GLYPH_ARENA_BYTES_SYNTH --
    // and why a reset on such a page is worth noticing. Rebuild with
    // -DLOG_LEVEL=3 to watch it.
    LOG_TRC("GFX", "Scaled-glyph cache reset (%u entries, %u bytes used)", scaledGlyphCount_, scaledGlyphUsed_);
    invalidateScaledGlyphCache();
  }

  uint8_t* const mask = scaledGlyphArena_.get() + scaledGlyphUsed_;
  memset(mask, 0, bytes);
  ScaledGlyphEntry& e = scaledGlyphEntries_[scaledGlyphCount_++];
  e.fontData = fontData;
  e.cp = static_cast<uint16_t>(cp);
  e.scaleBits = floatBits(scale);
  e.offset = scaledGlyphUsed_;
  e.sel = sel;
  e.w = static_cast<uint8_t>(w);
  e.h = static_cast<uint8_t>(h);
  scaledGlyphUsed_ = static_cast<uint16_t>(scaledGlyphUsed_ + bytes);
  return mask;
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

enum class TextRotation { None, Rotated90CW };

// =============================================================================
// Fast-path glyph rendering helpers (1-bit BW fonts, TextRotation::None)
// =============================================================================
//
// OVERVIEW
// --------
// The legacy path called drawPixel() once per set glyph pixel.  drawPixel()
// invokes rotateCoordinates() (a switch), does a bounds check, logs on OOB,
// then writes one bit.  For a typical 10×14 UI glyph that is ~100 calls.
//
// This fast path eliminates drawPixel() entirely by writing directly to the
// framebuffer in up to 8-pixel chunks via writeRowBits().
//
// FRAMEBUFFER LAYOUT
// ------------------
// 1 bpp, MSB-first, DISPLAY_WIDTH (800) pixels per row stored in
// DISPLAY_WIDTH_BYTES (100) bytes.  Bit 7 of byte 0 = leftmost pixel of
// row 0.  "Physical row" phyY occupies bytes [phyY*100 .. phyY*100+99].
// A set bit (1) is WHITE; a cleared bit (0) is BLACK.
//
// LANDSCAPE ORIENTATIONS  (2.5–3.1× speedup vs legacy)
// -------------------------------------------------------
// phyX and phyY are both linear functions of glyphX/glyphY in these modes,
// so each glyph row maps directly to a physical framebuffer row.
//
//   LandscapeCounterClockwise:  phyX = screenXBase+glyphX,  phyY = screenYBase+glyphY
//   LandscapeClockwise:         phyX = W-1-screenXBase-glyphX, phyY = H-1-screenYBase-glyphY
//
// Strategy: outer loop over glyphY (one physical row per iteration), inner
// loop reads 8-pixel chunks of that glyph row with bitmapExtract() and writes
// them with writeRowBits().  Bitmap access is purely sequential — fastest.
// LandscapeClockwise iterates glyph chunks right-to-left and applies
// reverseBits8() to flip horizontal direction.
//
// PORTRAIT ORIENTATIONS  (~2× speedup vs legacy)
// -----------------------------------------------
// Portrait (90° CW panel rotation):
//   phyX = screenYBase+glyphY,  phyY = H-1-screenXBase-glyphX
// PortraitInverted (90° CCW panel rotation):
//   phyX = W-1-screenYBase-glyphY, phyY = screenXBase+glyphX
//
// Here glyph COLUMNS map to physical rows.  Naively iterating column-by-column
// reads the bitmap with stride glyphWidth — cache-unfriendly and one bit at a
// time.  Instead we use an 8×8 bit-matrix transpose:
//
//   For each 8-row × 8-column glyph block:
//     1. Read 8 consecutive glyph rows (sequential bitmap access) into the
//        top 8 bytes of a uint64_t (one bitmapExtract per row).
//     2. Call transpose8x8() — an O(log 8) butterfly transform — to swap
//        the role of rows and columns in 3 passes of XOR-masking.
//     3. The resulting uint64_t holds 8 column bytes: byte k contains the
//        bits for glyph column glyphX+k, one per physical row, MSB-aligned.
//     4. Write each column byte with writeRowBits() to its physical row.
//
// For PortraitInverted the glyph rows are packed in reverse order (last row
// at MSB of the uint64_t) before transposing.  This ensures the post-transpose
// column bytes are already correctly ordered (MSB = leftmost phyX) without any
// per-column bit-reversal step.
//
// PARAMETERS
// ----------
//   screenXBase = cursorX + glyph.left  (logical X of glyph pixel [0,0])
//   screenYBase = cursorY - glyph.top   (logical Y of glyph pixel [0,0])

// Reverse all 8 bits of a byte (bit 7 ↔ bit 0).
static inline uint8_t reverseBits8(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

// Transpose an 8×8 bit matrix packed into a uint64_t.
//
// Input layout (row-major, row 0 at MSB):
//   bit (63 - 8*r - c)  =  matrix[r][c]   (r=row 0..7, c=col 0..7)
//
// After transposition:
//   bit (63 - 8*c - r)  =  matrix[r][c]
//   i.e. byte k = bits [63-8k .. 56-8k] holds column k, MSB = row 0.
//
// Uses the classic 3-pass butterfly (Warren, "Hacker's Delight" §7-3):
//   pass 1 swaps adjacent bit-pairs across a stride of 7 (nibble level),
//   pass 2 swaps across stride 14 (byte level),
//   pass 3 swaps across stride 28 (half-word level).
static inline uint64_t transpose8x8(uint64_t x) {
  uint64_t t;
  t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
  x ^= t ^ (t << 7);
  t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
  x ^= t ^ (t << 14);
  t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
  x ^= t ^ (t << 28);
  return x;
}

// Extract up to 8 bits from a 1-bit MSB-first packed bitmap starting at bit
// position 'bitPos'.  Returns them MSB-aligned (bit 7 = first extracted bit);
// the lower (8-count) bits are zeroed.
// All 'count' bits must lie within the valid bitmap byte range.
static inline uint8_t bitmapExtract(const uint8_t* bitmap, const int bitPos, const int count) {
  const int byteIdx = bitPos >> 3;
  const int bitOff = bitPos & 7;
  uint8_t result;
  if (bitOff == 0) {
    result = bitmap[byteIdx];
  } else if (count <= 8 - bitOff) {
    result = bitmap[byteIdx] << bitOff;  // all bits inside first byte
  } else {
    result = (uint8_t)(((uint16_t)bitmap[byteIdx] << 8 | bitmap[byteIdx + 1]) >> (8 - bitOff));
  }
  if (count < 8) result &= static_cast<uint8_t>(0xFF << (8 - count));
  return result;
}

// ---------------------------------------------------------------------------
// Fast glyph render pipeline
// ---------------------------------------------------------------------------
// Both 1-bit (BW) and 2-bit (antialiased) paths share the same structure:
//
//   gather → [reindex] → scatter
//
// The glyph bitmap is a row-major 2D tensor [glyphHeight][glyphWidth].
// The framebuffer is a row-major 2D tensor [DISPLAY_HEIGHT][DISPLAY_WIDTH_BYTES]
// (1 bpp) with a fixed row stride of DISPLAY_WIDTH_BYTES bytes.
//
// Non-rotated (Landscape): glyph rows map 1-to-1 to framebuffer rows.
// Reindex is a no-op; the pipeline is a tight per-row gather+scatter loop.
//
// Rotated 90° (Portrait): glyph rows become framebuffer columns.
// A row↔column axis swap (reindex) is required before scattering.
//
// 1-bit pipeline
//   gather  : extractGlyphBlock        reads an 8×8 glyph tile into a
//                                      contiguous uint64_t block
//                                      (≈ glyphTensor[tile].contiguous())
//   reindex : transpose8x8             swaps row↔column axes in the uint64_t;
//                                      pure index transform, no data movement
//   scatter : scatterBlockToFrameBuffer → writeRowBits
//                                      writes each column-byte to its row
//
// 2-bit pipeline (why it differs)
//   The glyph stores 4 gray levels (0–3). Rendering reduces these to a 1-bit
//   draw/skip decision via a render-mode threshold. That reduction is
//   information-lossy, so gather and threshold cannot be separated — there is
//   no contiguous 2-bit block to transpose. The two steps are fused:
//
//   gather+threshold : build2BitRowMask  Landscape — samples along glyph X
//                      build2BitColMask  Portrait  — samples along glyph Y
//                      both return a 1-bit mask ready for writeRowBits
//   scatter          : writeRowBits      same atom as the 1-bit path
// ---------------------------------------------------------------------------

// Scatter atom: merges 8 MSB-aligned bits into the framebuffer row at physical bit offset phyBitPos.
// Shared by both pipelines (1-bit: via scatterBlockToFrameBuffer; 2-bit: called directly).
//   bits      — MSB-aligned; bit 7 = pixel at phyBitPos, lower (8-count) bits are zero.
//   phyBitPos — physical X of the MSB pixel; may be negative for left-edge partial chunks.
//   pixelState true → black (clear bits to 0), false → white (set bits to 1).
static inline void writeRowBits(uint8_t* const row, const int phyBitPos, const uint8_t bits, const bool pixelState,
                                const int widthBytes) {
  uint8_t effectiveBits = bits;
  int byteIdx;
  int shift;
  if (phyBitPos < 0) {
    // Chunk starts off-screen left: clip by shifting out the off-screen MSBs.
    // bits is MSB-aligned, so (bits << neg) discards the neg off-screen pixels
    // and leaves the on-screen pixels MSB-aligned starting at physical X=0.
    const int neg = -phyBitPos;
    if (neg >= 8) return;  // entire chunk is off-screen left
    effectiveBits = bits << neg;
    byteIdx = 0;
    shift = 0;
  } else {
    byteIdx = phyBitPos >> 3;
    shift = phyBitPos & 7;
  }
  if (pixelState) {
    row[byteIdx] &= ~(effectiveBits >> shift);
    if (shift > 0 && byteIdx + 1 < widthBytes) row[byteIdx + 1] &= ~(uint8_t)(effectiveBits << (8 - shift));
  } else {
    row[byteIdx] |= (effectiveBits >> shift);
    if (shift > 0 && byteIdx + 1 < widthBytes) row[byteIdx + 1] |= (uint8_t)(effectiveBits << (8 - shift));
  }
}

// 1-bit pipeline step 1 — gather: reads an up-to-8×8 tile from the glyph tensor
// ([glyphHeight][glyphWidth], 1 bpp, row stride = glyphWidth bits) into a contiguous uint64_t.
// Equivalent to glyphTensor[glyphY:+rowCount, glyphX:+colCount].contiguous().
// Byte 7 = first source row (MSB-aligned). reverseRows implements a negative-stride gather along Y
// (reads rows bottom-to-top), needed for PortraitInverted.
// Full pipeline: extractGlyphBlock (gather) → transpose8x8 (reindex) → scatterBlockToFrameBuffer (scatter).
static inline uint64_t extractGlyphBlock(const uint8_t* const bitmap, const int stride, const int glyphX,
                                         const int glyphY, const int rowCount, const int colCount,
                                         const bool reverseRows) {
  uint64_t pack = 0;
  int bitStart = glyphY * stride + glyphX;
  for (int n = 0; n < rowCount; n++, bitStart += stride) {
    const int slot = reverseRows ? (rowCount - 1 - n) : n;
    pack |= static_cast<uint64_t>(bitmapExtract(bitmap, bitStart, colCount)) << (56 - 8 * slot);
  }
  return pack;
}

// 1-bit pipeline step 3 — scatter: writes column-bytes of the transposed block into framebuffer rows.
// The framebuffer is a 2D tensor [DISPLAY_HEIGHT][DISPLAY_WIDTH_BYTES] with non-unit row stride;
// phyYStride=±1 selects the traversal direction along Y (positive = top-to-bottom, negative = inverted).
// Each column k maps to row (phyYBase + k*phyYStride) via writeRowBits.
static inline void scatterBlockToFrameBuffer(uint8_t* const frameBuffer, const uint64_t pack, const int colCount,
                                             const int phyYBase, const int phyYStride, const int phyBitPos,
                                             const bool pixelState, const int displayHeight, const int widthBytes) {
  for (int k = 0; k < colCount; k++) {
    const uint8_t cols_k = static_cast<uint8_t>(pack >> (56 - 8 * k));
    if (cols_k == 0) continue;
    const int phyY = phyYBase + k * phyYStride;
    if (phyY < 0 || phyY >= displayHeight) continue;
    writeRowBits(frameBuffer + phyY * widthBytes, phyBitPos, cols_k, pixelState, widthBytes);
  }
}

static void renderGlyphFastBW(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                              const int glyphHeight, const int screenXBase, const int screenYBase,
                              const bool pixelState, const GfxRenderer::Orientation orientation, const int displayWidth,
                              const int displayHeight, const int widthBytes) {
  switch (orientation) {
    case GfxRenderer::LandscapeCounterClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = screenYBase + glyphY;
        if (phyY < 0 || phyY >= displayHeight) continue;
        uint8_t* const row = frameBuffer + phyY * widthBytes;
        const int rowBitStart = glyphY * glyphWidth;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int count = std::min(8, glyphWidth - glyphX);
          const uint8_t gbyte = bitmapExtract(bitmap, rowBitStart + glyphX, count);
          if (gbyte == 0) continue;
          const int phyBitPos = screenXBase + glyphX;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, gbyte, pixelState, widthBytes);
        }
      }
      break;
    }

    case GfxRenderer::LandscapeClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = displayHeight - 1 - (screenYBase + glyphY);
        if (phyY < 0 || phyY >= displayHeight) continue;
        uint8_t* const row = frameBuffer + phyY * widthBytes;
        const int rowBitStart = glyphY * glyphWidth;
        for (int chunkEnd = glyphWidth - 1; chunkEnd >= 0; chunkEnd -= 8) {
          const int chunkStart = std::max(0, chunkEnd - 7);
          const int count = chunkEnd - chunkStart + 1;
          const uint8_t gbyte_fwd = bitmapExtract(bitmap, rowBitStart + chunkStart, count);
          const uint8_t gbyte = reverseBits8(gbyte_fwd >> (8 - count));
          if (gbyte == 0) continue;
          const int phyBitPos = displayWidth - 1 - screenXBase - chunkEnd;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, gbyte, pixelState, widthBytes);
        }
      }
      break;
    }

    case GfxRenderer::Portrait: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = screenYBase + glyphY;
        if (phyBitPos + rowCount <= 0 || phyBitPos >= displayWidth) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          const uint64_t pack =
              transpose8x8(extractGlyphBlock(bitmap, glyphWidth, glyphX, glyphY, rowCount, colCount, false));
          scatterBlockToFrameBuffer(frameBuffer, pack, colCount, displayHeight - 1 - screenXBase - glyphX, -1,
                                    phyBitPos, pixelState, displayHeight, widthBytes);
        }
      }
      break;
    }

    case GfxRenderer::PortraitInverted: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = displayWidth - 1 - screenYBase - (glyphY + rowCount - 1);
        if (phyBitPos + rowCount <= 0 || phyBitPos >= displayWidth) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          const uint64_t pack =
              transpose8x8(extractGlyphBlock(bitmap, glyphWidth, glyphX, glyphY, rowCount, colCount, true));
          scatterBlockToFrameBuffer(frameBuffer, pack, colCount, screenXBase + glyphX, 1, phyBitPos, pixelState,
                                    displayHeight, widthBytes);
        }
      }
      break;
    }
  }
}

// Read one pixel from a tightly-packed 2-bit-per-pixel glyph bitmap.
// The bitmap is a row-major tensor [glyphHeight][glyphWidth] with no row padding;
// its pixel-row stride equals glyphWidth.  pixelPosition = row * glyphWidth + col.
// Returns the raw font value: 0=white, 1=light-gray, 2=dark-gray, 3=black.
static inline uint8_t get2BitPixel(const uint8_t* const bitmap, const int pixelPosition) {
  return (bitmap[pixelPosition >> 2] >> ((3 - (pixelPosition & 3)) * 2)) & 0x3;
}

// Convenience overload using explicit row/col/stride (tensor element access).
static inline uint8_t get2BitPixel(const uint8_t* const bitmap, const int stride, const int row, const int col) {
  return get2BitPixel(bitmap, row * stride + col);
}

// Compute the runtime drawMask for a given render mode and text darkness.
// Bit N set ⇒ draw when raw 2-bit font value == N
// (raw: 0=white, 1=light gray, 2=dark gray, 3=black).
//
// BW always draws every non-white pixel (darkness has no effect).
// For grayscale modes, increasing darkness folds more AA shades into the
// "draw" set so text becomes progressively bolder. The default darkness=1
// keeps the historical behavior (MSB pass draws both AA shades, LSB pass
// draws only the dark AA shade).
//
// At "Maximum" (darkness>=3) the grayscale passes are suppressed entirely
// (drawMask 0x00). The BW pass already writes raw {1,2,3} as solid black,
// so AA pixels render as hard black with no gray-LUT softening — visibly
// darker than darkness=2 because the gray waveform is skipped.
//
//    darkness | GRAYSCALE_MSB           | GRAYSCALE_LSB
//    --------- ------------------------- -------------------------
//    0        | 0x02 (raw {1})          | 0x04 (raw {2})
//    1        | 0x06 (raw {1,2}) ←dflt  | 0x04 (raw {2})   ←dflt
//    2        | 0x06 (raw {1,2})        | 0x06 (raw {1,2})
//    3+       | 0x00 (none)             | 0x00 (none)
//
// ─── Worked example ────────────────────────────────────────────────────────
// Imagine a 2-bit antialiased glyph for the diagonal stroke of a letter 'A'.
// Each cell holds the raw font value at that pixel:
//
//   raw values             . . . 2 3       legend:
//                          . . 2 3 1         . = 0 (white, never drawn)
//                          . 2 3 1 .         1 = light gray AA
//                          2 3 1 . .         2 = dark gray AA
//                          3 1 . . .         3 = solid black (stroke core)
//
// Three render passes write to three independent planes; the panel's
// grayscale waveform combines the BW plane with (MSB,LSB) into 4 shades:
//
//   (MSB, LSB)  →  panel shade
//      (0,0)    →  white
//      (1,0)    →  light gray
//      (0,1)    →  dark gray
//      (1,1)    →  black
//
// Per-pixel result for each darkness level (●=black, ▓=dark gray,
// ░=light gray, ·=white):
//
//   darkness=0  Normal — true 4-level AA
//     . . . ▓ ●        raw=1 → (1,0) light gray
//     . . ▓ ● ░        raw=2 → (0,1) dark gray
//     . ▓ ● ░ .        raw=3 → BW black
//     ▓ ● ░ . .        Crisp edges, lightest stroke. Best for thin/serif fonts.
//     ● ░ . . .
//
//   darkness=1  Dark — historical default
//     . . . ● ●        raw=1 → (1,0) light gray (unchanged)
//     . . ● ● ░        raw=2 → (1,1) black  (was dark gray)
//     . ● ● ░ .        Dark-gray fringe collapses to black; light fringe
//     ● ● ░ . .        survives. Stroke core thickens by ~1px on the
//     ● ░ . . .        steep side of the slope.
//
//   darkness=2  Extra Dark — both AA shades go black
//     . . . ● ●        raw=1 → (1,1) black
//     . . ● ● ●        raw=2 → (1,1) black
//     . ● ● ● .        All AA pixels are pushed to "black" in the gray
//     ● ● ● . .        plane. The gray waveform still runs, so pixels
//     ● ● . . .        share the gray-pass voltage profile (slightly
//                      softer than Maximum).
//
//   darkness=3  Maximum — grayscale pass skipped entirely
//     . . . ● ●        Both grayscale drawMasks are 0x00; nothing is
//     . . ● ● ●        written to the (MSB,LSB) planes. The BW pass —
//     . ● ● ● .        which already writes raw {1,2,3} as solid black —
//     ● ● ● . .        is the only pass the panel sees, refreshed with
//     ● ● . . .        the hard FAST waveform. Visually identical pixel
//                      footprint to darkness=2 but driven harder, so
//                      strokes look noticeably bolder/blacker on the
//                      physical e-ink panel.
//
//   darkness=4  Lighter — both AA shades go to the LIGHT tone
//     . . . ░ ●        raw=1 → (1,0) light gray (unchanged)
//     . . ░ ● ░        raw=2 → (1,0) light gray (was dark gray)
//     . ░ ● ░ .        The mirror of darkness=2: instead of collapsing
//     ░ ● ░ . .        both fringes to the dark tone, both take the
//     ● ░ . . .        light one. Thins the apparent stroke without
//                      touching the panel waveform.
//
//     Numerically last because the value is PERSISTED (see
//     CrossPointSettings::TEXT_DARKNESS) — inserting it at 0 would
//     silently redefine every saved setting. It reads out of order in
//     the menu; that is the price of not rewriting users' choices.
// ───────────────────────────────────────────────────────────────────────────
// Mirrors CrossPointSettings::DARKNESS_LIGHT. Spelled out here because this
// library does not include the firmware's settings header.
static constexpr uint8_t kDarknessLighter = 4;

static inline uint8_t drawMaskFor2BitMode(const GfxRenderer::RenderMode mode, const uint8_t darkness) {
  if (mode == GfxRenderer::BW) return 0x0E;  // draw raw {1,2,3}
  // Before the >= 3 test, which would otherwise swallow this as Maximum: the
  // value sits past Maximum in the enum only because it is persisted.
  if (darkness == kDarknessLighter) {
    // Both AA shades take the light tone -> raw 1 and 2 both land on (1,0).
    return (mode == GfxRenderer::GRAYSCALE_MSB) ? 0x06 : 0x00;
  }
  if (darkness >= 3) return 0x00;  // skip grayscale entirely (Maximum)
  if (mode == GfxRenderer::GRAYSCALE_MSB) {
    return (darkness == 0) ? 0x02 : 0x06;
  }
  // GRAYSCALE_LSB
  return (darkness >= 2) ? 0x06 : 0x04;
}

// 2-bit pipeline — fused gather+threshold (X axis): the 2-bit analog of extractGlyphBlock, but
// gather and threshold are collapsed into one pass. The threshold (2-bit raw value → 1-bit on/off)
// is information-lossy, so no contiguous 2-bit intermediate block can be formed mid-pipeline.
// The resulting 1-bit mask feeds writeRowBits directly (scatter). build2BitColMask is the Y-axis counterpart.
//
// Templated on the drawMask byte (a non-type template parameter) so each render-mode/darkness
// combination compiles to its own specialization with the mask folded into a constant.
template <uint8_t drawMask>
static inline uint8_t build2BitRowMask(const uint8_t* const bitmap, const int rowStartPixel, const int glyphXStartOrEnd,
                                       const int count, const bool reverseXInChunk) {
  // drawMask uses raw 2-bit glyph values directly from font bitmaps:
  // raw 0=white, 1=light gray, 2=dark gray, 3=black.
  // Bit N set means: draw/update when raw==N.
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int logicalX = reverseXInChunk ? (glyphXStartOrEnd - i) : (glyphXStartOrEnd + i);
    const uint8_t raw = get2BitPixel(bitmap, rowStartPixel + logicalX);
    if ((drawMask >> raw) & 0x01) mask |= static_cast<uint8_t>(1u << (7 - i));
  }
  return mask;
}

// Fast-path 2-bit mask builder for 8 byte-aligned pixels.
//
// The 2-bit glyph bitmap stores 4 pixels per byte, MSB-first:
//   byte b = [p0.msb p0.lsb  p1.msb p1.lsb  p2.msb p2.lsb  p3.msb p3.lsb]
//
// For each drawMask the draw decision collapses to a two-bit boolean:
//   0x0E (raw ∈ {1,2,3}): msb | lsb
//   0x06 (raw ∈ {1,2}):   msb ^ lsb
//   0x04 (raw == 2):      msb & ~lsb
//   0x02 (raw == 1):      ~msb & lsb
//
// Derivation for one byte:
//   msb_bits = b & 0xAA  →  bits 7,5,3,1 hold p0.msb … p3.msb; bits 6,4,2,0 = 0
//   lsb_bits = (b & 0x55) << 1  →  same positions hold p0.lsb … p3.lsb
//   draw_bits = msb_bits OP lsb_bits  →  bits 7,5,3,1 are the per-pixel draw flags
//
// compact4: squeezes those 4 draw flags from bit positions 7,5,3,1
//   into the top nibble (bits 7,6,5,4 → pixels 0,1,2,3).
//
// Two bytes b0 (pixels 0–3) and b1 (pixels 4–7) are combined:
//   mask = compact4(draw(b0)) | (compact4(draw(b1)) >> 4)
//
// This avoids the 8-iteration per-pixel loop in build2BitRowMask and
// processes the full 8-pixel chunk in ~16 ALU ops instead of ~56.
// The caller is responsible for only calling this when pixelStart is
// 4-pixel (1-byte) aligned (pixelStart & 3 == 0) and count == 8.
template <uint8_t drawMask>
static inline uint8_t build2BitRowMaskFromTwoBytes(const uint8_t b0, const uint8_t b1) {
  const uint8_t msb0 = b0 & 0xAA;
  const uint8_t lsb0 = (b0 & 0x55) << 1;
  const uint8_t msb1 = b1 & 0xAA;
  const uint8_t lsb1 = (b1 & 0x55) << 1;

  uint8_t draw0, draw1;
  if constexpr (drawMask == 0x0E) {  // glyph BW: raw ∈ {1,2,3}
    draw0 = msb0 | lsb0;
    draw1 = msb1 | lsb1;
  } else if constexpr (drawMask == 0x07) {  // image BW: raw ∈ {0,1,2} (not white)
    draw0 = ~(msb0 & lsb0) & 0xAA;
    draw1 = ~(msb1 & lsb1) & 0xAA;
  } else if constexpr (drawMask == 0x06) {  // raw ∈ {1,2}
    draw0 = msb0 ^ lsb0;
    draw1 = msb1 ^ lsb1;
  } else if constexpr (drawMask == 0x04) {  // raw == 2 (dark gray)
    draw0 = msb0 & ~lsb0;
    draw1 = msb1 & ~lsb1;
  } else {  // drawMask == 0x02, raw == 1 (light gray)
    static_assert(drawMask == 0x02, "unsupported drawMask in build2BitRowMaskFromTwoBytes");
    draw0 = ~msb0 & lsb0;
    draw1 = ~msb1 & lsb1;
  }

  // Compact each nibble's draw flags from bit positions 7,5,3,1 → 7,6,5,4.
  auto compact4 = [](const uint8_t d) -> uint8_t {
    return (d & 0x80) | ((d & 0x20) << 1) | ((d & 0x08) << 2) | ((d & 0x02) << 3);
  };
  return compact4(draw0) | (compact4(draw1) >> 4);
}

// 2-bit pipeline — fused gather+threshold (Y axis): column-direction counterpart to build2BitRowMask.
// Samples count pixels down glyph column glyphX starting at row glyphYStart; reverseRows implements
// a negative-stride view along Y (reads bottom-to-top), needed for PortraitInverted.
template <uint8_t drawMask>
static inline uint8_t build2BitColMask(const uint8_t* const bitmap, const int glyphWidth, const int glyphX,
                                       const int glyphYStart, const int count, const bool reverseRows) {
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int row = reverseRows ? (glyphYStart + count - 1 - i) : (glyphYStart + i);
    const uint8_t raw = get2BitPixel(bitmap, glyphWidth, row, glyphX);
    if ((drawMask >> raw) & 0x01) mask |= static_cast<uint8_t>(1u << (7 - i));
  }
  return mask;
}

// Shared body for Portrait and PortraitInverted 2-bit rendering.
// inverted=false → Portrait (phyY counts down, phyBitPos counts up).
// inverted=true  → PortraitInverted (phyY counts up, phyBitPos counts down).
// Both template params are compile-time constants; all ternaries fold away.
// `frameBuffer` may be a strip scratch covering only rows [fbOriginY, fbOriginY+fbRows);
// the writer subtracts fbOriginY when indexing and drops rows outside the band.
// In non-strip mode the caller passes fbOriginY=0, fbRows=displayHeight, so the
// translation is a no-op and the existing absolute-row indexing is preserved.
template <uint8_t drawMask, bool inverted>
static void renderGlyphFast2BitPortrait(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                                        const int glyphHeight, const int screenXBase, const int screenYBase,
                                        const bool writeState, const int displayWidth, const int displayHeight,
                                        const int widthBytes, const int fbOriginY, const int fbRows) {
  for (int glyphX = 0; glyphX < glyphWidth; glyphX++) {
    const int phyY = inverted ? (screenXBase + glyphX) : (displayHeight - 1 - (screenXBase + glyphX));
    // Single unsigned compare drops both off-band rows (strip mode) and any
    // out-of-frame row (full-frame mode: fbOriginY=0, fbRows=displayHeight),
    // matching what the Landscape* cases above do.
    const int rowY = phyY - fbOriginY;
    if (static_cast<unsigned>(rowY) >= static_cast<unsigned>(fbRows)) continue;
    uint8_t* const row = frameBuffer + rowY * widthBytes;
    for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
      const int count = std::min(8, glyphHeight - glyphY);
      const uint8_t mask = build2BitColMask<drawMask>(bitmap, glyphWidth, glyphX, glyphY, count, inverted);
      if (mask == 0) continue;
      const int phyBitPos = inverted ? (displayWidth - 1 - screenYBase - (glyphY + count - 1)) : (screenYBase + glyphY);
      if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
      writeRowBits(row, phyBitPos, mask, writeState, widthBytes);
    }
  }
}

template <uint8_t drawMask>
static void renderGlyphFast2Bit(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                                const int glyphHeight, const int screenXBase, const int screenYBase,
                                const bool pixelState, const GfxRenderer::Orientation orientation,
                                const int displayWidth, const int displayHeight, const int widthBytes,
                                const int fbOriginY, const int fbRows) {
  // Non-rotated text fast path for 2-bit glyphs. Writes compact masks directly to framebuffer rows.
  // TextRotation::Rotated90CW keeps the legacy per-pixel fallback path for safety and readability.
  // BW (drawMask 0x0E) honors the caller's pixelState; grayscale passes always clear the bit.
  //
  // Tiled grayscale: `frameBuffer` may be a strip scratch with origin fbOriginY
  // and fbRows; we subtract the origin when indexing and clip rows outside the
  // band. The unsigned compare drops both off-band rows (strip mode) and any
  // out-of-frame row (full-frame mode) in one branch.
  const bool writeState = (drawMask == 0x0E) ? pixelState : false;

  switch (orientation) {
    case GfxRenderer::LandscapeCounterClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = screenYBase + glyphY;
        const int rowY = phyY - fbOriginY;
        if (static_cast<unsigned>(rowY) >= static_cast<unsigned>(fbRows)) continue;
        uint8_t* const row = frameBuffer + rowY * widthBytes;
        const int rowStartPixel = glyphY * glyphWidth;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int count = std::min(8, glyphWidth - glyphX);
          const int pixelStart = rowStartPixel + glyphX;
          uint8_t mask;
          if (count == 8 && (pixelStart & 3) == 0) {
            const int srcByteIdx = pixelStart >> 2;
            mask = build2BitRowMaskFromTwoBytes<drawMask>(bitmap[srcByteIdx], bitmap[srcByteIdx + 1]);
          } else {
            mask = build2BitRowMask<drawMask>(bitmap, rowStartPixel, glyphX, count, false);
          }
          if (mask == 0) continue;
          const int phyBitPos = screenXBase + glyphX;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, mask, writeState, widthBytes);
        }
      }
      break;
    }

    case GfxRenderer::LandscapeClockwise: {
      // Row-outer/chunk-inner: framebuffer rows are written at stride widthBytes
      // (phyY decreases as glyphY increases). Keeping row-outer preserves sequential access
      // within each row, which is more cache-friendly than the chunk-outer alternative.
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = displayHeight - 1 - (screenYBase + glyphY);
        const int rowY = phyY - fbOriginY;
        if (static_cast<unsigned>(rowY) >= static_cast<unsigned>(fbRows)) continue;
        uint8_t* const row = frameBuffer + rowY * widthBytes;
        const int rowStartPixel = glyphY * glyphWidth;
        for (int chunkEnd = glyphWidth - 1; chunkEnd >= 0; chunkEnd -= 8) {
          const int chunkStart = std::max(0, chunkEnd - 7);
          const int count = chunkEnd - chunkStart + 1;
          const int pixelStart = rowStartPixel + chunkStart;
          uint8_t mask;
          if (count == 8 && (pixelStart & 3) == 0) {
            const int srcByteIdx = pixelStart >> 2;
            mask = reverseBits8(build2BitRowMaskFromTwoBytes<drawMask>(bitmap[srcByteIdx], bitmap[srcByteIdx + 1]));
          } else {
            mask = build2BitRowMask<drawMask>(bitmap, rowStartPixel, chunkEnd, count, true);
          }
          if (mask == 0) continue;
          const int phyBitPos = displayWidth - 1 - screenXBase - chunkEnd;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, mask, writeState, widthBytes);
        }
      }
      break;
    }

    case GfxRenderer::Portrait:
      renderGlyphFast2BitPortrait<drawMask, false>(frameBuffer, bitmap, glyphWidth, glyphHeight, screenXBase,
                                                   screenYBase, writeState, displayWidth, displayHeight, widthBytes,
                                                   fbOriginY, fbRows);
      break;

    case GfxRenderer::PortraitInverted:
      renderGlyphFast2BitPortrait<drawMask, true>(frameBuffer, bitmap, glyphWidth, glyphHeight, screenXBase,
                                                  screenYBase, writeState, displayWidth, displayHeight, widthBytes,
                                                  fbOriginY, fbRows);
      break;
  }
}

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyphRef glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph.width;
  const uint8_t height = glyph.height;
  const int left = glyph.left;
  const int top = glyph.top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      // Compute the drawMask once per glyph from the current render mode + text-darkness setting.
      // The fast path dispatches on this at runtime to a template specialization so the mask is
      // a compile-time constant inside the inner loops.
      const uint8_t drawMask = drawMaskFor2BitMode(renderMode, renderer.getTextDarkness());

      // drawMask == 0 means "draw nothing" — used by Maximum darkness to skip grayscale passes.
      if (drawMask == 0) return;

      if constexpr (rotation == TextRotation::None) {
        // Fast path for normal text orientation. Handles all device orientations via renderGlyphFast2Bit.
        // Strip-aware: getWriteTarget() returns the band scratch when a strip is active, otherwise
        // the live framebuffer; the (fbOriginY, fbRows) pair tells the writer how to translate phyY
        // and clip rows outside the band.
        uint8_t* const fb = renderer.getWriteTarget();
        const int fbOriginY = renderer.getWriteOriginY();
        const int fbRows = renderer.getWriteRows();
        switch (drawMask) {
          case 0x0E:  // BW
            renderGlyphFast2Bit<0x0E>(fb, bitmap, width, height, innerBase, outerBase, pixelState,
                                      renderer.getOrientation(), renderer.getDisplayWidth(),
                                      renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), fbOriginY, fbRows);
            // Inline grayscale capture: the same glyph, blitted again into each
            // anti-aliasing plane with that plane's own draw mask. This is the
            // whole point of the capture — the page walk, the layout and the
            // glyph decode above happened ONCE, and re-running only the blit is
            // what makes a second and third full page render unnecessary.
            //
            // Masks come from drawMaskFor2BitMode() so the planes honour the
            // darkness setting exactly as the staged passes did. Darkness
            // "Maximum" yields 0x00 for both, which the guards below skip — the
            // BW pass has already drawn every AA pixel solid black there, which
            // is what that setting means.
            if (renderer.grayCaptureActive()) {
              const uint8_t msbMask = drawMaskFor2BitMode(GfxRenderer::GRAYSCALE_MSB, renderer.getTextDarkness());
              const uint8_t lsbMask = drawMaskFor2BitMode(GfxRenderer::GRAYSCALE_LSB, renderer.getTextDarkness());
              const int planeRows = static_cast<int>(renderer.getDisplayHeight());
              // Planes are full-panel, so origin 0 / full height regardless of
              // any band the framebuffer write is using.
              if (msbMask == 0x06) {
                renderGlyphFast2Bit<0x06>(renderer.grayCaptureMsb(), bitmap, width, height, innerBase, outerBase,
                                          pixelState, renderer.getOrientation(), renderer.getDisplayWidth(),
                                          renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), 0, planeRows);
              } else if (msbMask == 0x02) {
                renderGlyphFast2Bit<0x02>(renderer.grayCaptureMsb(), bitmap, width, height, innerBase, outerBase,
                                          pixelState, renderer.getOrientation(), renderer.getDisplayWidth(),
                                          renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), 0, planeRows);
              }
              if (lsbMask == 0x06) {
                renderGlyphFast2Bit<0x06>(renderer.grayCaptureLsb(), bitmap, width, height, innerBase, outerBase,
                                          pixelState, renderer.getOrientation(), renderer.getDisplayWidth(),
                                          renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), 0, planeRows);
              } else if (lsbMask == 0x04) {
                renderGlyphFast2Bit<0x04>(renderer.grayCaptureLsb(), bitmap, width, height, innerBase, outerBase,
                                          pixelState, renderer.getOrientation(), renderer.getDisplayWidth(),
                                          renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), 0, planeRows);
              }
            }
            break;
          case 0x06:  // raw {1,2}
            renderGlyphFast2Bit<0x06>(fb, bitmap, width, height, innerBase, outerBase, pixelState,
                                      renderer.getOrientation(), renderer.getDisplayWidth(),
                                      renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), fbOriginY, fbRows);
            break;
          case 0x04:  // raw {2}
            renderGlyphFast2Bit<0x04>(fb, bitmap, width, height, innerBase, outerBase, pixelState,
                                      renderer.getOrientation(), renderer.getDisplayWidth(),
                                      renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), fbOriginY, fbRows);
            break;
          case 0x02:  // raw {1}
            renderGlyphFast2Bit<0x02>(fb, bitmap, width, height, innerBase, outerBase, pixelState,
                                      renderer.getOrientation(), renderer.getDisplayWidth(),
                                      renderer.getDisplayHeight(), renderer.getDisplayWidthBytes(), fbOriginY, fbRows);
            break;
        }
        return;
      }

      // Rotated text fallback: per-pixel path. Uses the same drawMask as the fast path so darkness
      // takes effect uniformly. (Previously this branch had a separate X4-only "draw light gray too"
      // quirk; that quirk is now subsumed by the default darkness=1 mask, which already includes
      // both AA shades for the MSB pass.)
      const bool isBW = (drawMask == 0x0E);
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // raw value straight from the font: 0=white, 1=light gray, 2=dark gray, 3=black
          const uint8_t raw = (byte >> bit_index) & 0x3;

          if ((drawMask >> raw) & 0x01) {
            // BW honors caller's pixelState; grayscale passes always clear the bit (false)
            renderer.drawPixel(screenX, screenY, isBW ? pixelState : false);
          }
        }
      }
    } else {
      // Fast path: 1-bit BW mode, non-rotated text — byte-level framebuffer writes, no drawPixel() per pixel.
      // renderGlyphFastBW is NOT strip-aware (no fbOriginY/fbRows in its signature) and would
      // mis-index into the strip scratch as if it were the full framebuffer. Today no caller
      // activates a strip in BW mode, but route to the per-pixel fallback (drawPixel is
      // strip-aware) if that ever changes so we never hand a strip buffer to the fast helper.
      if constexpr (rotation == TextRotation::None) {
        if (renderMode == GfxRenderer::BW && !renderer.isStripActive()) {
          renderGlyphFastBW(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase, pixelState,
                            renderer.getOrientation(), renderer.getDisplayWidth(), renderer.getDisplayHeight(),
                            renderer.getDisplayWidthBytes());
          return;
        }
      }
      // Fallback: rotated text or non-BW render mode — per-pixel drawPixel().
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// Resample one glyph to `scale`, reporting every destination pixel that must be drawn to
// `emit(dstX, dstY)`. Single source of truth for both consumers: the cache builder (which
// records the pixels into a 1-bit mask) and the uncached fallback (which draws them
// straight to the framebuffer). Keeping one copy of the maths is what stops the two from
// drifting apart — the cached and uncached paths must produce identical pixels.
//
// `drawMask` is only consulted when scale >= 1; `minRaw2Bit` only when scale < 1.
template <typename Emit>
static inline void emitScaledGlyphPixels(const uint8_t* const bitmap, const bool is2Bit, const int srcW, const int srcH,
                                         const int dstW, const int dstH, const float scale, const uint8_t minRaw2Bit,
                                         const uint8_t drawMask, Emit&& emit) {
  // The ESP32-C3 has no FPU — every float op in a per-pixel loop is a soft-float call
  // costing hundreds of cycles. Both resampling paths therefore run in 16.16 fixed point:
  // the ONLY float operation per glyph is the one-time conversion of `scale` into a
  // fixed-point step; the pixel loops are pure integer (the C3's M extension gives a
  // hardware divider for the one division per upscaled pixel).
  constexpr int FP_SHIFT = 16;
  constexpr int32_t FP_ONE = 1 << FP_SHIFT;
  const int32_t invScaleFP = static_cast<int32_t>(FP_ONE / scale + 0.5f);

  // Per-source-pixel raw ink level: 2-bit fonts carry 4 AA levels (0..3); 1-bit fonts map
  // to 0 or 3. Shared by both resampling paths below.
  auto srcRaw = [&](const int sx, const int sy) -> uint8_t {
    const int pos = sy * srcW + sx;
    if (is2Bit) {
      const uint8_t byte = bitmap[pos >> 2];
      return (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
    }
    const uint8_t byte = bitmap[pos >> 3];
    return ((byte >> (7 - (pos & 7))) & 1) ? 3 : 0;
  };

  // Downscaling (small-caps fold, sup/sub, shrunken inline sizes) stays on the crisp
  // nearest-neighbor point-sample: shrinking already suppresses jaggies, and area-averaging
  // would only blur thin strokes below the 50% threshold and drop them. Downscalers pass
  // minRaw2Bit=2 to keep only the dark shades. srcX/srcY advance by a fixed-point step
  // instead of the historical per-pixel float division (identical floor semantics).
  if (scale < 1.0f) {
    const uint8_t minRaw1Bit = 1;  // 1-bit fonts: any ink draws
    int32_t srcYFP = 0;
    for (int dstY = 0; dstY < dstH; dstY++, srcYFP += invScaleFP) {
      const int srcY = srcYFP >> FP_SHIFT;
      if (srcY >= srcH) break;
      int32_t srcXFP = 0;
      for (int dstX = 0; dstX < dstW; dstX++, srcXFP += invScaleFP) {
        const int srcX = srcXFP >> FP_SHIFT;
        if (srcX >= srcW) break;
        const uint8_t raw = srcRaw(srcX, srcY);
        if (raw >= (is2Bit ? minRaw2Bit : minRaw1Bit)) {
          emit(dstX, dstY);
        }
      }
    }
    return;
  }

  // Upscaling (heading scale, e.g. h1=1.6×): area-weighted coverage resampling that preserves the
  // font's grayscale AA. Nearest-neighbor bloated each source pixel into a scale×scale solid block,
  // so light-gray AA edge pixels (raw==1, 33% coverage) rendered as full-black blocks — the jagged,
  // over-bold look.
  //
  // Here each destination pixel integrates the fractional source coverage over the source region it
  // maps back to (0..1), then re-quantizes that coverage into a 2-bit AA level (0=white .. 3=black).
  // That reconstructed level feeds the SAME multi-pass machinery body text uses: drawMaskFor2BitMode
  // decides, per render pass and darkness setting, whether this level draws into the current plane.
  // In grayscale render modes the enlarged heading therefore emits true gray edge pixels (MSB/LSB
  // planes) instead of a hard 1-bit threshold; in BW mode every non-white level draws, matching the
  // body font's weight. (The caller derives the pixel state from the same mask: grayscale passes
  // clear the bit, only the BW pass honors the caller's pixelState.)
  //
  // Area integration in fixed point. Overlap extents are 16.16 and bounded by
  // invScaleFP <= FP_ONE (scale >= 1); the raw x h x w product needs 64 bits, which
  // RV32IM handles with a few integer multiplies — still an order of magnitude cheaper
  // than one soft-float op. Verified pixel-equivalent to the historical float path
  // (differences only at mathematically exact quantization ties, ~0.2% of AA edge
  // pixels, where the float result was itself rounding-dependent).
  const int64_t dstPixelAreaFP = static_cast<int64_t>(invScaleFP) * invScaleFP;
  int32_t srcY0FP = 0;
  for (int dstY = 0; dstY < dstH; dstY++, srcY0FP += invScaleFP) {
    const int32_t srcY1FP = srcY0FP + invScaleFP;
    const int sy0 = srcY0FP >> FP_SHIFT;
    const int sy1 = std::min(static_cast<int>((srcY1FP - 1) >> FP_SHIFT), srcH - 1);
    int32_t srcX0FP = 0;
    for (int dstX = 0; dstX < dstW; dstX++, srcX0FP += invScaleFP) {
      const int32_t srcX1FP = srcX0FP + invScaleFP;
      const int sx0 = srcX0FP >> FP_SHIFT;
      const int sx1 = std::min(static_cast<int>((srcX1FP - 1) >> FP_SHIFT), srcW - 1);

      int64_t covered = 0;
      for (int sy = sy0; sy <= sy1; sy++) {
        const int32_t hOverlapFP = std::min(srcY1FP, static_cast<int32_t>(sy + 1) << FP_SHIFT) -
                                   std::max(srcY0FP, static_cast<int32_t>(sy) << FP_SHIFT);
        if (hOverlapFP <= 0) continue;
        for (int sx = sx0; sx <= sx1; sx++) {
          const uint8_t raw = srcRaw(sx, sy);
          if (raw == 0) continue;
          const int32_t wOverlapFP = std::min(srcX1FP, static_cast<int32_t>(sx + 1) << FP_SHIFT) -
                                     std::max(srcX0FP, static_cast<int32_t>(sx) << FP_SHIFT);
          if (wOverlapFP <= 0) continue;
          covered += static_cast<int64_t>(raw) * hOverlapFP * wOverlapFP;
        }
      }
      // covered / dstPixelArea is 3x the ink fraction (raw already carries the x3 of the
      // source encoding); round to the nearest level 0..3. Mirrors the source font's raw
      // encoding so the drawMask logic is identical to the body-text per-pixel path.
      const uint8_t raw = static_cast<uint8_t>(std::min<int64_t>(3, (covered + dstPixelAreaFP / 2) / dstPixelAreaFP));
      if ((drawMask >> raw) & 0x01) {
        emit(dstX, dstY);
      }
    }
  }
}

// Render a glyph at an arbitrary scale factor.
// Used for heading font-size scaling (e.g. h1=1.6×, h2=1.4×, h3=1.2×) and per-word inline
// sizes, including superscript/subscript (which carry an explicit size percentage instead
// of a hardwired 50% — the SUP/SUB style bits only shift the baseline). It is also the path
// every word of body text takes when the book's stylesheet sets a font-size on it, which is
// why the resampled mask is cached (see GfxRenderer::ScaledGlyphEntry).
// Upscaling (scale > 1.0) uses area-weighted coverage resampling so enlarged headings stay
// anti-aliased and keep the body font's visual weight instead of the jagged, over-bold look
// nearest-neighbor produced. Downscaling (scale < 1.0) keeps crisp nearest-neighbor point-
// sampling.
// Emit one scaled glyph into a full-panel 1-bpp anti-aliasing plane.
//
// The unscaled fast path captures the planes at its 2-bit dispatch (see the
// `case 0x0E:` block). Without the same hook here, every glyph drawn at a scale
// != 1.0 -- which the scaled-glyph mask cache above notes is "every word of any
// book whose stylesheet puts a font-size on body text, which is most of them" --
// reached the panel with no entry in either plane. Single-push pages therefore
// carried strictly less anti-aliasing than the staged passes they replaced,
// which DID grey scaled text: renderCharAtScale takes renderMode and derives its
// own drawMask, so a GRAYSCALE_MSB/LSB re-render greyed these glyphs.
//
// Mirrors the cached/uncached split of the framebuffer path but never consults
// the strip: the planes are always whole-panel, so there is no band to translate
// into. Downscaling never reaches here -- it thresholds against minRaw2Bit and
// has no AA levels to lift in any path, staged or captured.
static void captureScaledGlyphPlane(const GfxRenderer& renderer, uint8_t* const plane, const uint8_t drawMask,
                                    const uint8_t* const bitmap, const bool is2Bit, const int srcW, const int srcH,
                                    const int dstW, const int dstH, const float scale, const uint8_t minRaw2Bit,
                                    const int baseX, const int baseY, const EpdFontData* const fontData,
                                    const uint32_t cp) {
  if (plane == nullptr || drawMask == 0) return;  // 0x00 == "Maximum" darkness: no grayscale at all

  const uint8_t* mask = renderer.findScaledGlyphMask(fontData, cp, scale, drawMask, dstW, dstH);
  if (!mask) {
    if (uint8_t* slot = renderer.allocScaledGlyphMask(fontData, cp, scale, drawMask, dstW, dstH)) {
      emitScaledGlyphPixels(bitmap, is2Bit, srcW, srcH, dstW, dstH, scale, minRaw2Bit, drawMask,
                            [slot, dstW](const int dstX, const int dstY) {
                              const int bit = dstY * dstW + dstX;
                              slot[bit >> 3] |= static_cast<uint8_t>(0x80 >> (bit & 7));
                            });
      mask = slot;
    }
  }
  if (mask) {
    // pixelState=false makes writeRowBits OR the bits in, which is how a plane
    // marks a pixel -- the same convention the staged grayscale passes used.
    renderGlyphFastBW(plane, mask, dstW, dstH, baseX, baseY, false, renderer.getOrientation(),
                      renderer.getDisplayWidth(), renderer.getDisplayHeight(), renderer.getDisplayWidthBytes());
    return;
  }

  // Mask arena exhausted: write the plane bits directly, with the same rotation
  // and clipping drawPixel() applies to the framebuffer.
  emitScaledGlyphPixels(bitmap, is2Bit, srcW, srcH, dstW, dstH, scale, minRaw2Bit, drawMask,
                        [&renderer, plane, baseX, baseY](const int dstX, const int dstY) {
                          int phyX = 0;
                          int phyY = 0;
                          const int w = renderer.getDisplayWidth();
                          const int h = renderer.getDisplayHeight();
                          rotateCoordinates(renderer.getOrientation(), baseX + dstX, baseY + dstY, &phyX, &phyY, w, h);
                          if (phyX < 0 || phyX >= w || phyY < 0 || phyY >= h) return;
                          plane[static_cast<uint32_t>(phyY) * renderer.getDisplayWidthBytes() + (phyX / 8)] |=
                              static_cast<uint8_t>(1u << (7 - (phyX % 8)));
                        });
}

static void renderCharAtScale(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                              const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                              const bool pixelState, const EpdFontFamily::Style style, const float scale,
                              const uint8_t minRaw2Bit = 1) {
  const EpdGlyphRef glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph.width;
  const int srcH = glyph.height;
  if (srcW <= 0 || srcH <= 0) return;

  const int dstW = static_cast<int>(srcW * scale + 0.5f);
  const int dstH = static_cast<int>(srcH * scale + 0.5f);
  if (dstW <= 0 || dstH <= 0) return;

  const int baseX = cursorX + static_cast<int>(glyph.left * scale + 0.5f);
  const int baseY = cursorY - static_cast<int>(glyph.top * scale + 0.5f);
  const bool is2Bit = fontData->is2Bit;

  // Downscaling thresholds raw ink levels against minRaw2Bit and always honors the
  // caller's pixelState; upscaling reconstructs a 2-bit AA level and lets the render
  // mode's drawMask decide, with grayscale passes always clearing the bit. `sel` is
  // whichever of the two varies the emitted pixel set, and so belongs in the cache key.
  uint8_t drawMask = 0x0E;
  bool writeState = pixelState;
  if (scale >= 1.0f) {
    drawMask = drawMaskFor2BitMode(renderMode, renderer.getTextDarkness());
    if (drawMask == 0) return;  // Maximum darkness suppresses grayscale passes entirely
    writeState = (drawMask == 0x0E) ? pixelState : false;
  }
  const uint8_t sel = (scale < 1.0f) ? minRaw2Bit : drawMask;

  // Capture BEFORE the framebuffer draw below: the cached path returns straight
  // out of it, so anything placed after that return runs only on the fallback.
  if (renderMode == GfxRenderer::BW && scale >= 1.0f && renderer.grayCaptureActive()) {
    const uint8_t darkness = renderer.getTextDarkness();
    captureScaledGlyphPlane(renderer, renderer.grayCaptureMsb(),
                            drawMaskFor2BitMode(GfxRenderer::GRAYSCALE_MSB, darkness), bitmap, is2Bit, srcW, srcH, dstW,
                            dstH, scale, minRaw2Bit, baseX, baseY, fontData, cp);
    captureScaledGlyphPlane(renderer, renderer.grayCaptureLsb(),
                            drawMaskFor2BitMode(GfxRenderer::GRAYSCALE_LSB, darkness), bitmap, is2Bit, srcW, srcH, dstW,
                            dstH, scale, minRaw2Bit, baseX, baseY, fontData, cp);
  }

  // Cached path: resample once per distinct (glyph, scale, sel), then draw every
  // occurrence with the same 8-pixel-chunk blitter unscaled text uses. renderGlyphFastBW
  // is not strip-aware, so a strip target falls through to the per-pixel path below
  // (drawPixel is strip-aware) — same reason the unscaled 1-bit path guards on it.
  if (!renderer.isStripActive()) {
    // fontData already identifies the style variant (getData and getGlyph share
    // getFont(style)), so the style bits are not part of the key.
    const uint8_t* mask = renderer.findScaledGlyphMask(fontData, cp, scale, sel, dstW, dstH);
    if (!mask) {
      if (uint8_t* slot = renderer.allocScaledGlyphMask(fontData, cp, scale, sel, dstW, dstH)) {
        emitScaledGlyphPixels(bitmap, is2Bit, srcW, srcH, dstW, dstH, scale, minRaw2Bit, drawMask,
                              [slot, dstW](const int dstX, const int dstY) {
                                const int bit = dstY * dstW + dstX;
                                slot[bit >> 3] |= static_cast<uint8_t>(0x80 >> (bit & 7));
                              });
        mask = slot;
      }
    }
    if (mask) {
      renderGlyphFastBW(renderer.getFrameBuffer(), mask, dstW, dstH, baseX, baseY, writeState,
                        renderer.getOrientation(), renderer.getDisplayWidth(), renderer.getDisplayHeight(),
                        renderer.getDisplayWidthBytes());
      return;
    }
  }

  // Uncached fallback: a strip target is active, the glyph is too large to cache, or the
  // arena could not be allocated. Emit straight to the framebuffer, one pixel at a time.
  emitScaledGlyphPixels(bitmap, is2Bit, srcW, srcH, dstW, dstH, scale, minRaw2Bit, drawMask,
                        [&renderer, baseX, baseY, writeState](const int dstX, const int dstY) {
                          renderer.drawPixel(baseX + dstX, baseY + dstY, writeState);
                        });
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;
  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();

  // Note: this call should be inlined for better performance
  rotateCoordinates(getOrientation(), x, y, &phyX, &phyY, displayWidth, displayHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= displayWidth || phyY < 0 || phyY >= displayHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  uint8_t* target = frameBuffer;
  const uint32_t rowY = static_cast<uint32_t>(phyY);

  // Calculate byte position and bit position
  const uint32_t byteIndex = rowY * getDisplayWidthBytes() + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

bool GfxRenderer::getPixel(const int x, const int y) const {
  int phyX = 0;
  int phyY = 0;
  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();
  rotateCoordinates(getOrientation(), x, y, &phyX, &phyY, displayWidth, displayHeight);
  if (phyX < 0 || phyX >= displayWidth || phyY < 0 || phyY >= displayHeight) return false;

  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * getDisplayWidthBytes() + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);
  // A CLEARED bit is dark: drawPixel(state=true) clears it.
  return (frameBuffer[byteIndex] & (1 << bitPosition)) == 0;
}

size_t GfxRenderer::readFramebufferRegion(const int x, const int y, const int width, const int height,
                                          uint8_t* const buffer, const size_t bufferSize) const {
  if (!buffer || width <= 0 || height <= 0) return 0;
  const size_t needed = (static_cast<size_t>(width) * static_cast<size_t>(height) + 7) / 8;
  if (needed > bufferSize) return 0;
  if (x < 0 || y < 0 || x + width > getScreenWidth() || y + height > getScreenHeight()) return 0;

  memset(buffer, 0, needed);
  size_t bit = 0;
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++, bit++) {
      if (getPixel(x + col, y + row)) buffer[bit / 8] |= static_cast<uint8_t>(1u << (7 - (bit % 8)));
    }
  }
  return needed;
}

void GfxRenderer::writeFramebufferRegion(const int x, const int y, const int width, const int height,
                                         const uint8_t* const buffer) const {
  if (!buffer || width <= 0 || height <= 0) return;
  if (x < 0 || y < 0 || x + width > getScreenWidth() || y + height > getScreenHeight()) return;

  size_t bit = 0;
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++, bit++) {
      const bool dark = (buffer[bit / 8] & (1u << (7 - (bit % 8)))) != 0;
      drawPixel(x + col, y + row, dark);
    }
  }
}

void GfxRenderer::writePhysicalPortraitPackedRow(const int physicalY, const uint8_t* packedRow, const int pixelWidth,
                                                 const bool invertBits) const {
  if (!frameBuffer || !packedRow || pixelWidth <= 0 || physicalY < 0 || physicalY >= static_cast<int>(panelWidth)) {
    return;
  }

  const int visiblePixels = std::min(pixelWidth, static_cast<int>(panelHeight));
  const int controllerX = physicalY;
  const int byteCol = controllerX >> 3;
  const uint8_t dstMask = static_cast<uint8_t>(0x80u >> (controllerX & 7));

  for (int physicalX = 0; physicalX < visiblePixels; physicalX++) {
    uint8_t bit = static_cast<uint8_t>((packedRow[physicalX >> 3] >> (7 - (physicalX & 7))) & 1);
    if (invertBits) bit ^= 1;
    const int controllerY = static_cast<int>(panelHeight) - 1 - physicalX;
    uint8_t* const dst = frameBuffer + static_cast<uint32_t>(controllerY) * panelWidthBytes + byteCol;
    if (bit) {
      *dst |= dstMask;
    } else {
      *dst &= static_cast<uint8_t>(~dstMask);
    }
  }
}

// The base scale is applied at the PUBLIC boundary only, and the guard below is not an
// optimisation: `scale == 1.0f` returns the integer untouched, so every font that ships real faces
// measures and draws exactly as it did before this existed. That matters because the pipeline
// goldens are byte-identical by invariant -- a float round-trip on the common path would break
// them for no reason.
static inline int applyBase(const int v, const float scale) {
  return scale == 1.0f ? v : static_cast<int>(v * scale + 0.5f);
}

int GfxRenderer::rawTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return 0;
  }

  int w = 0, h = 0;
  family->getTextDimensions(text, &w, &h, style);
  return w;
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  return applyBase(rawTextWidth(fontId, text, style), fontBaseScale(fontId));
}

bool GfxRenderer::getTextInkMetrics(const int fontId, const char* text, const EpdFontFamily::Style style,
                                    int* aboveBaseline, int* belowBaseline) const {
  *aboveBaseline = 0;
  *belowBaseline = 0;
  if (text == nullptr || *text == '\0') return false;
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) return false;

  bool any = false;
  uint32_t cp;
  const char* p = text;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p)))) {
    const EpdGlyphRef glyph = family->getGlyph(cp, style);
    if (!glyph) continue;
    // glyph.top: baseline to bitmap top edge; ink below baseline = height - top.
    *aboveBaseline = std::max(*aboveBaseline, static_cast<int>(glyph.top));
    *belowBaseline = std::max(*belowBaseline, static_cast<int>(glyph.height) - static_cast<int>(glyph.top));
    any = true;
  }
  // Ink extents are pixel metrics like every other accessor here, so a synthesised size must
  // report its own, not its master's.
  const float base = fontBaseScale(fontId);
  if (base != 1.0f) {
    *aboveBaseline = static_cast<int>(*aboveBaseline * base + 0.5f);
    *belowBaseline = static_cast<int>(*belowBaseline * base + 0.5f);
  }
  return any;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  // A synthesised size has no faces of its own, so the unscaled blitter below cannot draw it --
  // it walks raw glyph advances, which belong to the master. Hand it to the resampling path at
  // the font's own scale. Real faces (the overwhelming majority) skip this with one comparison.
  {
    const float base = fontBaseScale(fontId);
    if (base != 1.0f) {
      drawTextAtScale(fontId, x, y, text, black, style, base);
      return;
    }
  }
  const int yPos = y + getFontAscenderSize(fontId);
  const int screenWidth = getScreenWidth();
  const int screenHeight = getScreenHeight();
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int lastBaseAdvanceFP = 0;  // 12.4 fixed-point
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = *family;
  const auto renderModeSnapshot = getRenderMode();

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyphRef combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph.top, combiningGlyph.height, lastBaseTop);
      const int combiningX =
          combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, combiningGlyph.left, combiningGlyph.width);
      renderCharImpl<TextRotation::None>(*this, renderModeSnapshot, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Small-caps: fold lowercase to its uppercase glyph and draw it scaled.  Decided
    // per glyph so already-uppercase letters, digits and punctuation stay full-size.
    const bool smallCapsStyle = (style & EpdFontFamily::SMALL_CAPS) != 0;
    const bool folded = smallCapsStyle && smallCaps::fold(cp);

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      auto kernFP = static_cast<int32_t>(font.getKerning(prevCp, cp, style));  // 4.4 fixed-point kern
      if (folded) kernFP = static_cast<int32_t>(kernFP * smallCaps::SCALE + 0.5f);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);  // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyphRef glyph = font.getGlyph(cp, style);
    if (!glyph) {
      lastBaseX += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      lastBaseAdvanceFP = 0;
      continue;
    }

    // Folded glyphs render at smallCaps::SCALE, so layout metrics scale to match.
    const int effLeft = folded ? static_cast<int>(glyph.left * smallCaps::SCALE) : glyph.left;
    const int effWidth = folded ? static_cast<int>(glyph.width * smallCaps::SCALE + 0.5f) : glyph.width;
    const int effHeight = folded ? static_cast<int>(glyph.height * smallCaps::SCALE + 0.5f) : glyph.height;
    const int effTop = folded ? static_cast<int>(glyph.top * smallCaps::SCALE) : glyph.top;

    lastBaseLeft = effLeft;
    lastBaseWidth = effWidth;
    lastBaseTop = effTop;
    lastBaseAdvanceFP = glyph.advanceX;

    // SUP/SUB glyph scaling is no longer applied here: superscript/subscript words carry
    // an explicit per-word size percentage (ChapterHtmlSlimParser), so they arrive via
    // drawTextScaled. The SUP/SUB style bits only shift the baseline in TextBlock::render.
    if (folded) {
      lastBaseAdvanceFP = static_cast<int>(lastBaseAdvanceFP * smallCaps::SCALE + 0.5f);
    }
    prevAdvanceFP = lastBaseAdvanceFP;

    // Skip rasterization for glyphs fully outside the logical viewport.
    // This avoids expensive per-pixel bounds checks and noisy OOB logs when
    // long lines overflow past the right edge.
    const int glyphX = lastBaseX + effLeft;
    const int glyphY = yPos - effTop;
    const bool glyphOffscreen = (effWidth <= 0 || effHeight <= 0 || glyphX >= screenWidth || glyphY >= screenHeight ||
                                 glyphX + effWidth <= 0 || glyphY + effHeight <= 0);
    if (glyphOffscreen) {
      prevCp = cp;
      continue;
    }

    if (folded) {
      renderCharAtScale(*this, renderModeSnapshot, font, cp, lastBaseX, yPos, black, style, smallCaps::SCALE,
                        /*minRaw2Bit=*/2);
    } else {
      renderCharImpl<TextRotation::None>(*this, renderModeSnapshot, font, cp, lastBaseX, yPos, black, style);
    }
    prevCp = cp;
  }
}

// Public entry: `scale` is the CSS/heading residual, compounded with the font's base scale so a
// heading at 1.6em inside synthesised 24 pt body text is ONE resample at 1.2 x 1.6, not two.
void GfxRenderer::drawTextScaled(const int fontId, const int x, const int y, const char* text, const bool black,
                                 const EpdFontFamily::Style style, const float scale) const {
  // A non-positive scale has always meant "draw as is" here, not "draw nothing". Keep it: the
  // first version of this wrapper returned instead, which silently changed behaviour for any
  // caller passing 0 to mean unscaled.
  const float total = (scale <= 0.0f ? 1.0f : scale) * fontBaseScale(fontId);
  if (total > 0.99f && total < 1.01f) {
    // Exactly 1 after compounding: the unscaled blitter is both faster and sharper. Only reachable
    // when the font's base is 1, so drawText() cannot bounce back here.
    drawText(fontId, x, y, text, black, style);
    return;
  }
  drawTextAtScale(fontId, x, y, text, black, style, total);
}

void GfxRenderer::drawTextAtScale(const int fontId, const int x, const int y, const char* text, const bool black,
                                  const EpdFontFamily::Style style, const float scale) const {
  if (text == nullptr || *text == '\0') return;

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) return;
  const auto& font = *family;
  const auto renderModeSnapshot = getRenderMode();

  // RAW: `scale` already carries the font's base, so the public accessor would apply it twice.
  const int yPos = y + static_cast<int>(rawFontAscenderSize(fontId) * scale + 0.5f);
  int32_t cursorFP = x << 4;  // 12.4 fixed-point

  const bool smallCapsStyle = (style & EpdFontFamily::SMALL_CAPS) != 0;
  // Sup/sub glyphs keep the crisp dark-shade threshold the dedicated 50% sampler used:
  // dropping the light-gray AA level stops small raised/lowered digits going muddy.
  const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
  uint32_t cp;
  uint32_t prevCp = 0;
  const char* p = text;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p)))) {
    // Small-caps composes with the heading scale: folded glyphs render at scale*SCALE.
    const bool folded = smallCapsStyle && smallCaps::fold(cp);
    const float effScale = folded ? scale * smallCaps::SCALE : scale;

    const EpdGlyphRef glyph = font.getGlyph(cp, style);
    if (!glyph) {
      prevCp = cp;
      continue;
    }

    // Apply kerning scaled
    if (prevCp) {
      int kern = font.getKerning(prevCp, cp, style);
      cursorFP += static_cast<int>(kern * effScale + 0.5f);
    }

    const int cursorX = (cursorFP + 8) >> 4;
    renderCharAtScale(*this, renderModeSnapshot, font, cp, cursorX, yPos, black, style, effScale,
                      /*minRaw2Bit=*/(folded || isSupSub) ? 2 : 1);

    const int scaledAdvanceFP = static_cast<int>(glyph.advanceX * effScale + 0.5f);
    cursorFP += scaledAdvanceFP;
    prevCp = cp;
  }
}

// The three *Scaled accessors take the CSS/heading residual and compound it with the font's own
// base scale, from the RAW measure -- going through the public accessor would apply the base twice.
int GfxRenderer::getTextWidthScaled(const int fontId, const char* text, const EpdFontFamily::Style style,
                                    const float scale) const {
  return static_cast<int>(rawTextWidth(fontId, text, style) * fontBaseScale(fontId) * scale + 0.5f);
}

int GfxRenderer::getLineHeightScaled(const int fontId, const float scale) const {
  return static_cast<int>(rawLineHeight(fontId) * fontBaseScale(fontId) * scale + 0.5f);
}

int GfxRenderer::getFontAscenderSizeScaled(const int fontId, const float scale) const {
  return static_cast<int>(rawFontAscenderSize(fontId) * fontBaseScale(fontId) * scale + 0.5f);
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();

  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    // In Portrait/PortraitInverted a logical vertical line maps to a physical horizontal span.
    switch (getOrientation()) {
      case Portrait:
        fillPhysicalHSpan(displayHeight - 1 - x1, y1, y2, state);
        return;
      case PortraitInverted:
        fillPhysicalHSpan(x1, displayWidth - 1 - y2, displayWidth - 1 - y1, state);
        return;
      default:
        for (int y = y1; y <= y2; y++) drawPixel(x1, y, state);
        return;
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    // In Landscape a logical horizontal line maps to a physical horizontal span.
    switch (getOrientation()) {
      case LandscapeCounterClockwise:
        fillPhysicalHSpan(y1, x1, x2, state);
        return;
      case LandscapeClockwise:
        fillPhysicalHSpan(displayHeight - 1 - y1, displayWidth - 1 - x2, displayWidth - 1 - x1, state);
        return;
      default:
        for (int x = x1; x <= x2; x++) drawPixel(x, y1, state);
        return;
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - i, y + i, state);
    drawLine(x + width - i, y + i, x + width - i, y + height - i, state);
    drawLine(x + width - i, y + height - i, x + i, y + height - i, state);
    drawLine(x + i, y + height - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    if (width > 0) {
      fillRect(left, py, width, 1, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

// Write a patterned horizontal span directly into the physical framebuffer with byte-level operations.
// patternByte is repeated across the full span; partial edge bytes are blended with existing content.
// Bit layout: MSB-first (bit 7 = phyX=0, bit 0 = phyX=7); 0 bits = dark pixel, 1 bits = white pixel.
void GfxRenderer::fillPhysicalHSpanByte(const int phyY, const int phyX_start, const int phyX_end,
                                        const uint8_t patternByte) const {
  const int cX0 = std::max(phyX_start, 0);
  const int cX1 = std::min(phyX_end, (int)getDisplayWidth() - 1);
  if (cX0 > cX1 || phyY < 0 || phyY >= (int)getDisplayHeight()) return;

  uint8_t* target = frameBuffer;
  const int rowY = phyY;

  uint8_t* const row = target + rowY * getDisplayWidthBytes();
  const int startByte = cX0 >> 3;
  const int endByte = cX1 >> 3;
  const int leftBits = cX0 & 7;   // first bit index within startByte
  const int rightBits = cX1 & 7;  // last bit index within endByte

  if (startByte == endByte) {
    // Both endpoints in the same byte
    const uint8_t fillMask = (0xFF >> leftBits) & ~(0xFF >> (rightBits + 1));
    row[startByte] = (row[startByte] & ~fillMask) | (patternByte & fillMask);
    return;
  }

  // Left partial byte
  if (leftBits != 0) {
    const uint8_t fillMask = 0xFF >> leftBits;
    row[startByte] = (row[startByte] & ~fillMask) | (patternByte & fillMask);
  }

  // Full bytes in the middle
  const int fullStart = (leftBits == 0) ? startByte : startByte + 1;
  const int fullEnd = (rightBits == 7) ? endByte : endByte - 1;
  if (fullStart <= fullEnd) {
    memset(row + fullStart, patternByte, fullEnd - fullStart + 1);
  }

  // Right partial byte
  if (rightBits != 7) {
    const uint8_t fillMask = ~(0xFF >> (rightBits + 1));
    row[endByte] = (row[endByte] & ~fillMask) | (patternByte & fillMask);
  }
}

// Thin wrapper: state=true → 0x00 (all dark), false → 0xFF (all white).
void GfxRenderer::fillPhysicalHSpan(const int phyY, const int phyX_start, const int phyX_end, const bool state) const {
  fillPhysicalHSpanByte(phyY, phyX_start, phyX_end, state ? 0x00 : 0xFF);
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  if (width <= 0 || height <= 0) return;

  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();

  // For each orientation, one logical dimension maps to a constant physical row, allowing the
  // perpendicular dimension to be written as a byte-level span — eliminating per-pixel overhead.
  switch (getOrientation()) {
    case Portrait:
      // Logical column x → physical row (displayHeight-1-x); logical y range → physical x span
      for (int lx = x; lx < x + width; lx++) {
        fillPhysicalHSpan(displayHeight - 1 - lx, y, y + height - 1, state);
      }
      return;
    case PortraitInverted:
      // Logical column x → physical row x; logical y range → physical x span (mirrored)
      for (int lx = x; lx < x + width; lx++) {
        fillPhysicalHSpan(lx, displayWidth - 1 - (y + height - 1), displayWidth - 1 - y, state);
      }
      return;
    case LandscapeCounterClockwise:
      // Logical row y → physical row y; logical x range → physical x span
      for (int ly = y; ly < y + height; ly++) {
        fillPhysicalHSpan(ly, x, x + width - 1, state);
      }
      return;
    case LandscapeClockwise:
      // Logical row y → physical row (displayHeight-1-y); logical x range → physical x span (mirrored)
      for (int ly = y; ly < y + height; ly++) {
        fillPhysicalHSpan(displayHeight - 1 - ly, displayWidth - 1 - (x + width - 1), displayWidth - 1 - x, state);
      }
      return;
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  if (color == Color::Clear) {
  } else if (color == Color::Black) {
    fillRect(x, y, width, height, true);
  } else if (color == Color::White) {
    fillRect(x, y, width, height, false);
  } else if (color == Color::DarkGray) {
    // Pattern: dark where (phyX + phyY) % 2 == 0 (alternating checkerboard).
    // Byte patterns (phyY even / phyY odd):
    //   Portrait / PortraitInverted: 0xAA / 0x55
    //   LandscapeCW / LandscapeCCW: 0x55 / 0xAA
    switch (getOrientation()) {
      case Portrait:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = getDisplayHeight() - 1 - lx;
          const uint8_t pb = (phyY % 2 == 0) ? 0xAA : 0x55;
          fillPhysicalHSpanByte(phyY, y, y + height - 1, pb);
        }
        return;
      case PortraitInverted:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = lx;
          const uint8_t pb = (phyY % 2 == 0) ? 0xAA : 0x55;
          fillPhysicalHSpanByte(phyY, getDisplayWidth() - 1 - (y + height - 1), getDisplayWidth() - 1 - y, pb);
        }
        return;
      case LandscapeCounterClockwise:
        for (int ly = y; ly < y + height; ly++) {
          const int phyY = ly;
          const uint8_t pb = (phyY % 2 == 0) ? 0x55 : 0xAA;
          fillPhysicalHSpanByte(phyY, x, x + width - 1, pb);
        }
        return;
      case LandscapeClockwise:
        for (int ly = y; ly < y + height; ly++) {
          const int phyY = getDisplayHeight() - 1 - ly;
          const uint8_t pb = (phyY % 2 == 0) ? 0x55 : 0xAA;
          fillPhysicalHSpanByte(phyY, getDisplayWidth() - 1 - (x + width - 1), getDisplayWidth() - 1 - x, pb);
        }
        return;
    }
  } else if (color == Color::LightGray) {
    // Pattern: dark where phyX % 2 == 0 && phyY % 2 == 0 (1-in-4 pixels dark).
    // Byte patterns (phyY even / phyY odd) — 0xFF rows write no dark pixels and are skipped:
    //   Portrait:         0xFF (skip) / 0x55
    //   PortraitInverted: 0xAA        / 0xFF (skip)
    //   LandscapeCCW:     0x55        / 0xFF (skip)
    //   LandscapeCW:      0xFF (skip) / 0xAA
    switch (getOrientation()) {
      case Portrait:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = getDisplayHeight() - 1 - lx;
          if (phyY % 2 == 0) continue;  // all-white row — no dark pixels to write
          fillPhysicalHSpanByte(phyY, y, y + height - 1, 0x55);
        }
        return;
      case PortraitInverted:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = lx;
          if (phyY % 2 != 0) continue;  // all-white row
          fillPhysicalHSpanByte(phyY, getDisplayWidth() - 1 - (y + height - 1), getDisplayWidth() - 1 - y, 0xAA);
        }
        return;
      case LandscapeCounterClockwise:
        for (int ly = y; ly < y + height; ly++) {
          const int phyY = ly;
          if (phyY % 2 != 0) continue;  // all-white row
          fillPhysicalHSpanByte(phyY, x, x + width - 1, 0x55);
        }
        return;
      case LandscapeClockwise:
        for (int ly = y; ly < y + height; ly++) {
          const int phyY = getDisplayHeight() - 1 - ly;
          if (phyY % 2 == 0) continue;  // all-white row
          fillPhysicalHSpanByte(phyY, getDisplayWidth() - 1 - (x + width - 1), getDisplayWidth() - 1 - x, 0xAA);
        }
        return;
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  const auto currentOrientation = getOrientation();
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(currentOrientation, x, y, &rotatedX, &rotatedY, getDisplayWidth(), getDisplayHeight());
  // Rotate origin corner
  switch (currentOrientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  display.drawImageTransparent(bitmap, y, getScreenWidth() - width - x, height, width);
}

void GfxRenderer::drawIconInverted(const uint8_t bitmap[], const int x, const int y, const int width,
                                   const int height) const {
  // Portrait-mode coordinate transform (x↔y swap), matching drawIcon.
  // OR with ~srcByte sets framebuffer bits to 1 (white) wherever the icon
  // bitmap is 0 (black) — produces a white icon on a black background.
  const int physX = y;
  const int physY = getScreenWidth() - width - x;
  const int imgW = height;  // dimensions swapped by portrait transform
  const int imgH = width;
  const int srcStride = (imgW + 7) / 8;

  if (physX + imgW <= 0 || physX >= static_cast<int>(panelWidthBytes) * 8) return;
  if (physY + imgH <= 0 || physY >= static_cast<int>(panelHeight)) return;

  const int baseByte = (physX >= 0) ? (physX >> 3) : -(((-physX) + 7) >> 3);
  const int bitShift = ((physX % 8) + 8) % 8;

  const int trail = srcStride * 8 - imgW;
  const uint8_t trailMask = static_cast<uint8_t>(0xFF << trail);
  const int lastCol = srcStride - 1;

  for (int row = 0; row < imgH; ++row) {
    const int destY = physY + row;
    if (destY < 0 || destY >= static_cast<int>(panelHeight)) continue;
    const int rowBase = destY * static_cast<int>(panelWidthBytes);
    const int srcOffset = row * srcStride;

    if (bitShift == 0) {
      for (int col = 0; col < srcStride; ++col) {
        const int dst = baseByte + col;
        if (dst < 0) continue;
        if (dst >= static_cast<int>(panelWidthBytes)) break;
        uint8_t inv = ~bitmap[srcOffset + col];
        if (col == lastCol && trail > 0) inv &= trailMask;
        frameBuffer[rowBase + dst] |= inv;
      }
    } else {
      const int rsh = bitShift;
      const int lsh = 8 - bitShift;
      for (int col = 0; col < srcStride; ++col) {
        uint8_t inv = ~bitmap[srcOffset + col];
        if (col == lastCol && trail > 0) inv &= trailMask;
        const int dstHi = baseByte + col;
        const int dstLo = dstHi + 1;
        if (dstHi >= 0 && dstHi < static_cast<int>(panelWidthBytes)) {
          frameBuffer[rowBase + dstHi] |= static_cast<uint8_t>(inv >> rsh);
        }
        if (dstLo >= 0 && dstLo < static_cast<int>(panelWidthBytes)) {
          frameBuffer[rowBase + dstLo] |= static_cast<uint8_t>(inv << lsh);
        }
      }
    }
  }
}

// =============================================================================
// Fast-path bitmap rendering helpers
// =============================================================================
//
// These mirror the glyph fast-path strategy (renderGlyphFastBW / renderGlyphFast2Bit*):
// instead of calling drawPixel() once per pixel (rotate + bounds-check + 1-bit RMW),
// we write up to 8 pixels at a time directly to the framebuffer using writeRowBits.
//
// Source data: readNextRow always produces a 2-bit packed row (4px/byte, MSB-first,
// raw values 0=white 1=light-gray 2=dark-gray 3=black).  The same
// build2BitRowMaskFromTwoBytes / build2BitRowMask / build2BitColMask helpers used
// for 2-bit glyphs apply here with no changes.
//
// Fast path is taken only when !isScaled.  Scaled images fall through to the
// per-pixel drawPixel loop unchanged.
//
// LANDSCAPE orientations
//   phyY = f(screenY)  — constant per bitmap row
//   phyX = g(screenX)  — linear in bmpX
// → each bitmap row maps to a contiguous slice of one physical framebuffer row.
//   Outer loop: bmpY (one row ptr per iteration).
//   Inner loop: 8-pixel chunks via build2BitRowMaskFromTwoBytes (aligned) or
//               build2BitRowMask (partial edges) → writeRowBits.
//
// PORTRAIT orientations
//   phyX = f(screenY)  — constant per bitmap row
//   phyY = g(screenX)  — linear in bmpX
// → each bitmap row maps to a single physical column (one phyX bit across multiple rows).
//   Outer loop: bmpY (computes phyX and the framebuffer bit-mask once).
//   Inner loop: bmpX — reads one 2-bit pixel, writes one bit to the correct physical row.
//   This eliminates rotateCoordinates(), the bounds-check log, and redundant byte/bit
//   recomputation from drawPixel(), giving ~2× on the inner loop.
//   (The 8×8 transpose used for glyphs would give ~8× but requires all rows in RAM
//   simultaneously; bitmap rows are read sequentially from the BMP file, so they
//   cannot be buffered for transposition without a separate heap allocation.
//   The column loop is the practical optimum for the streaming-read model.)
//
// =============================================================================

// Dispatch helper: write one row's worth of 2-bit pixels from 'outputRow' into the
// framebuffer row at physical Y 'phyY'.  'pixelStartX' is the logical source X offset
// (non-zero when cropPixX > 0).  'count' is the number of pixels to write.
// 'screenXOrigin' is the physical X of source pixel pixelStartX.
// For LandscapeCounterClockwise: physical X increases with bmpX.
// For LandscapeClockwise:        physical X decreases with bmpX (reversed).
template <uint8_t drawMask>
static void bitmapWriteLandscapeRow(uint8_t* const frameBuffer, const uint8_t* const outputRow, const int pixelStartX,
                                    const int count, const int phyY, const int screenXOrigin, const bool reverseX,
                                    const bool pixelState, const int displayWidth, const int displayHeight,
                                    const int widthBytes) {
  if (phyY < 0 || phyY >= displayHeight) return;
  uint8_t* const row = frameBuffer + phyY * widthBytes;
  // Walk in 8-pixel chunks.  For reverseX (LandscapeClockwise) we iterate
  // right-to-left through source pixels, mirroring renderGlyphFastBW.
  if (!reverseX) {
    for (int dx = 0; dx < count; dx += 8) {
      const int chunkCount = std::min(8, count - dx);
      const int srcPixel = pixelStartX + dx;  // index into the 2-bit packed row
      uint8_t mask;
      // Fast path: aligned 8-pixel chunk uses the two-byte SIMD helper
      if (chunkCount == 8 && (srcPixel & 3) == 0) {
        mask = build2BitRowMaskFromTwoBytes<drawMask>(outputRow[srcPixel >> 2], outputRow[(srcPixel >> 2) + 1]);
      } else {
        mask = build2BitRowMask<drawMask>(outputRow, 0, srcPixel, chunkCount, false);
      }
      if (mask == 0) continue;
      const int phyBitPos = screenXOrigin + dx;
      if (phyBitPos + chunkCount <= 0 || phyBitPos >= displayWidth) continue;
      writeRowBits(row, phyBitPos, mask, pixelState, widthBytes);
    }
  } else {
    // LandscapeClockwise: source pixel 0 maps to the rightmost physical X.
    // Iterate source chunks right-to-left, reverse bits, write to correct phyBitPos.
    for (int chunkEnd = count - 1; chunkEnd >= 0; chunkEnd -= 8) {
      const int chunkStart = std::max(0, chunkEnd - 7);
      const int chunkCount = chunkEnd - chunkStart + 1;
      const int srcPixel = pixelStartX + chunkStart;
      uint8_t mask_fwd;
      if (chunkCount == 8 && (srcPixel & 3) == 0) {
        mask_fwd = build2BitRowMaskFromTwoBytes<drawMask>(outputRow[srcPixel >> 2], outputRow[(srcPixel >> 2) + 1]);
      } else {
        mask_fwd = build2BitRowMask<drawMask>(outputRow, 0, srcPixel, chunkCount, false);
      }
      if (mask_fwd == 0) continue;
      const uint8_t mask = reverseBits8(mask_fwd >> (8 - chunkCount));
      // screenXOrigin is the physical X of source pixel (count-1); chunkEnd counts from that end
      const int phyBitPos = screenXOrigin - chunkEnd;
      if (phyBitPos + chunkCount <= 0 || phyBitPos >= displayWidth) continue;
      writeRowBits(row, phyBitPos, mask, pixelState, widthBytes);
    }
  }
}

// Portrait fast path: one bitmap row (bmpY) maps to one physical column (phyX = const).
// Writes one bit per bmpX pixel into its physical row.  Saves rotateCoordinates() and
// bounds-check log overhead compared to drawPixel(), with no extra heap allocation.
template <uint8_t drawMask>
static void bitmapWritePortraitColumn(uint8_t* const frameBuffer, const uint8_t* const outputRow, const int pixelStartX,
                                      const int count, const int phyX, const int phyYOrigin, const int phyYStride,
                                      const bool pixelState, const int displayHeight, const int widthBytes) {
  if (phyX < 0 || phyX >= widthBytes * 8) return;
  const int byteCol = phyX >> 3;
  const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (phyX & 7));
  for (int dx = 0; dx < count; dx++) {
    const int srcPixel = pixelStartX + dx;
    const uint8_t raw = (outputRow[srcPixel >> 2] >> ((3 - (srcPixel & 3)) * 2)) & 0x3;
    if (!((drawMask >> raw) & 0x01)) continue;
    const int phyY = phyYOrigin + dx * phyYStride;
    if (phyY < 0 || phyY >= displayHeight) continue;
    uint8_t* const bytePtr = frameBuffer + phyY * widthBytes + byteCol;
    if (pixelState) {
      *bytePtr &= ~bitMask;  // black
    } else {
      *bytePtr |= bitMask;  // white
    }
  }
}

// Core bitmap fast-path dispatcher: called once per bitmap row.
// orientation, drawMask and pixelState are resolved before the row loop.
template <uint8_t drawMask>
static void bitmapFastRow(uint8_t* const frameBuffer, const uint8_t* const outputRow, const int cropPixX,
                          const int renderWidth, const int screenX, const int screenY,
                          const GfxRenderer::Orientation orientation, const bool pixelState, const int displayWidth,
                          const int displayHeight, const int widthBytes) {
  switch (orientation) {
    case GfxRenderer::LandscapeCounterClockwise:
      // phyX = screenX + (bmpX - cropPixX),  phyY = screenY
      bitmapWriteLandscapeRow<drawMask>(frameBuffer, outputRow, cropPixX, renderWidth, screenY, screenX, false,
                                        pixelState, displayWidth, displayHeight, widthBytes);
      break;

    case GfxRenderer::LandscapeClockwise:
      // phyX = displayWidth-1 - screenX - (bmpX-cropPixX),  phyY = displayHeight-1-screenY
      // screenXOrigin for reversed walk = physical X of source pixel (renderWidth-1)
      bitmapWriteLandscapeRow<drawMask>(frameBuffer, outputRow, cropPixX, renderWidth, displayHeight - 1 - screenY,
                                        displayWidth - 1 - screenX, true, pixelState, displayWidth, displayHeight,
                                        widthBytes);
      break;

    case GfxRenderer::Portrait:
      // phyX = screenY,  phyY = displayHeight-1 - screenX - (bmpX-cropPixX),  phyYStride = -1
      bitmapWritePortraitColumn<drawMask>(frameBuffer, outputRow, cropPixX, renderWidth, screenY,
                                          displayHeight - 1 - screenX, -1, pixelState, displayHeight, widthBytes);
      break;

    case GfxRenderer::PortraitInverted:
      // phyX = displayWidth-1-screenY,  phyY = screenX + (bmpX-cropPixX),  phyYStride = +1
      bitmapWritePortraitColumn<drawMask>(frameBuffer, outputRow, cropPixX, renderWidth, displayWidth - 1 - screenY,
                                          screenX, 1, pixelState, displayHeight, widthBytes);
      break;
  }
}

void GfxRenderer::drawGray8Pixel(const Gray8Target& gray8, const int x, const int y, const uint8_t gray) const {
  int phyX = 0;
  int phyY = 0;
  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();
  rotateCoordinates(getOrientation(), x, y, &phyX, &phyY, displayWidth, displayHeight);
  if (phyX < 0 || phyX >= displayWidth || phyY < 0 || phyY >= displayHeight) return;
  gray8.canvas[static_cast<uint32_t>(phyY) * gray8.stride + phyX] = gray;
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY, const Gray8Target* gray8) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));
  // One row of 8-bit samples, only when a canvas is actually being painted.
  // Stack allocation is not an option: a row is one image width, thousands of
  // bytes, and the ESP32-C3 stack budget for a call frame is 256.
  auto* gray8Row = gray8 ? static_cast<uint8_t*>(malloc(bitmap.getWidth())) : nullptr;

  if (!outputRow || !rowBytes || (gray8 && !gray8Row)) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    free(outputRow);
    free(rowBytes);
    free(gray8Row);
    return;
  }

  const auto renderModeSnapshot = getRenderMode();
  const auto orientation = getOrientation();
  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();
  const int widthBytes = getDisplayWidthBytes();
  // Width of the rendered (post-crop) region in source pixels
  const int renderWidth = bitmap.getWidth() - 2 * cropPixX;

  // Pixel write state for each render mode:
  //   BW: black pixels (state=true clears the bit)
  //   GRAYSCALE_*: white=false sets the bit in the AA plane
  const bool pixelState = (renderModeSnapshot == BW);

  // Image raw values: 0=black, 1=dark-gray, 2=light-gray, 3=white  (opposite of glyph convention).
  // BW draws raw {0,1,2} = mask 0x07.
  // Grayscale: image raws 1 and 2 are swapped vs glyphs, so the LSB mask must draw raw 1
  // (image dark-gray) not raw 2 (image light-gray).  MSB draws both grays in both conventions.
  //   GRAYSCALE_MSB: draw raw {1,2} → 0x06
  //   GRAYSCALE_LSB: draw raw {1}   → 0x02  (dark-gray sets LSB → panel dark gray)
  // This matches the slow path (val==1||val==2 for MSB; val==1 for LSB).
  uint8_t drawMask;
  if (renderModeSnapshot == BW) {
    drawMask = 0x07;
  } else if (renderModeSnapshot == GRAYSCALE_MSB) {
    drawMask = 0x06;
  } else {
    drawMask = 0x02;  // GRAYSCALE_LSB: image raw 1 = dark-gray
  }

  // Absolute planes carry the whole image rather than a mask over the B/W base,
  // so a value outside this plane's set has to be written CLEAR, not skipped.
  // The row blitter writes one polarity per call, so the fast path cannot
  // express that in a single pass; take the per-pixel path instead of paying
  // four more template instantiations of it in a build already within ~177 KB
  // of filling its flash partition. Nothing is lost by it: the absolute
  // waveform this feeds runs for about a second, so the pixel loop is not what
  // the pass waits on.
  const bool absolutePlanes = renderModeSnapshot != BW && getAbsoluteGrayPlanes();

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes, gray8Row) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      free(gray8Row);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    if (!gray8 && !isScaled && !absolutePlanes && drawMask != 0x00) {
      // Fast path: write up to 8 pixels per call directly to the framebuffer.
      switch (drawMask) {
        case 0x07:
          bitmapFastRow<0x07>(frameBuffer, outputRow, cropPixX, renderWidth, x, screenY, orientation, pixelState,
                              displayWidth, displayHeight, widthBytes);
          break;
        case 0x06:
          bitmapFastRow<0x06>(frameBuffer, outputRow, cropPixX, renderWidth, x, screenY, orientation, pixelState,
                              displayWidth, displayHeight, widthBytes);
          break;
        case 0x04:
          bitmapFastRow<0x04>(frameBuffer, outputRow, cropPixX, renderWidth, x, screenY, orientation, pixelState,
                              displayWidth, displayHeight, widthBytes);
          break;
        case 0x02:
          bitmapFastRow<0x02>(frameBuffer, outputRow, cropPixX, renderWidth, x, screenY, orientation, pixelState,
                              displayWidth, displayHeight, widthBytes);
          break;
        default:
          bitmapFastRow<0x07>(frameBuffer, outputRow, cropPixX, renderWidth, x, screenY, orientation, pixelState,
                              displayWidth, displayHeight, widthBytes);
          break;
      }
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      if (gray8) {
        // The whole point of this path: store the sample, let the panel quantise.
        drawGray8Pixel(*gray8, screenX, screenY, gray8Row[bmpX]);
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (absolutePlanes) {
        // (LSB,MSB) is 00 black, 10 dark, 01 light, 11 white: the LSB plane
        // holds white|dark-gray, the MSB plane white|light-gray. Every covered
        // pixel is written, which also keeps the image's edge exact under
        // scaling -- the caller cannot pre-fill a rectangle for it, because the
        // fitted extent it computes rounds where the loop below floors.
        const bool bit = (renderModeSnapshot == GRAYSCALE_LSB) ? (val == 1 || val == 3) : (val == 2 || val == 3);
        drawPixel(screenX, screenY, !bit);  // state=false sets the bit
      } else if (renderModeSnapshot == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderModeSnapshot == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderModeSnapshot == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  free(outputRow);
  free(rowBytes);
  free(gray8Row);
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0) {
    const float s = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    if (s != 1.0f) {
      scale = s;
      isScaled = true;
    }
  }
  if (maxHeight > 0) {
    const float s = static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight());
    if (s < scale || (scale == 1.0f && s != 1.0f)) {
      scale = s;
      isScaled = (scale != 1.0f);
    }
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow). Sized for
  // Bitmap::expandOneBitRow, which writes whole source bytes: a byte more than readNextRow needs
  // at some widths.
  const int outputRowSize = 2 * ((bitmap.getWidth() + 7) / 8);
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

  const auto orientation = getOrientation();
  const int displayWidth = getDisplayWidth();
  const int displayHeight = getDisplayHeight();
  const int widthBytes = getDisplayWidthBytes();

  // ── Unscaled fast path (scale == 1.0): draw each source row 1:1. ──────────────
  //
  // Rows are read a batch at a time and expanded to the 2-bit packing through a table built once
  // for the image (Bitmap::oneBitExpansion), instead of readNextRow()'s per-pixel tone, adjust and
  // quantise -- the same bytes, as a 1-bit image's pixels depend on their palette index alone.
  // On the device that conversion was ~60% of a cover's draw and the per-row SD read another
  // 20-40% (X3 and T5S3 logs, 2026-10-01). Without the memory for the batch it falls back to
  // reading row by row, as before.
  if (!isScaled) {
    const int srcW = bitmap.getWidth();
    const int srcH = bitmap.getHeight();
    const int srcRowBytes = bitmap.getRowBytes();
    constexpr int kBatchBytes = 2048;
    const int batchRows = std::max(1, std::min(srcH, kBatchBytes / std::max(1, srcRowBytes)));
    auto* table = static_cast<uint16_t*>(malloc(256 * sizeof(uint16_t)));
    auto* batch = static_cast<uint8_t*>(malloc(static_cast<size_t>(batchRows) * srcRowBytes));
    if (table != nullptr && batch != nullptr) {
      bitmap.oneBitExpansion(table);
      for (int bmpY = 0; bmpY < srcH; bmpY += batchRows) {
        const int rows = std::min(batchRows, srcH - bmpY);
        if (bitmap.readRawRows(batch, rows) != BmpReaderError::Ok) {
          LOG_ERR("GFX", "Failed to read rows %d-%d from 1-bit bitmap", bmpY, bmpY + rows - 1);
          break;
        }
        for (int r = 0; r < rows; ++r) {
          const int bmpYOffset = bitmap.isTopDown() ? bmpY + r : srcH - 1 - (bmpY + r);
          const int screenY = y + bmpYOffset;
          if (screenY < 0 || screenY >= getScreenHeight()) continue;
          Bitmap::expandOneBitRow(table, batch + static_cast<size_t>(r) * srcRowBytes, outputRow, srcW);
          // BW only (1-bit images are never rendered in grayscale passes)
          bitmapFastRow<0x07>(frameBuffer, outputRow, 0, srcW, x, screenY, orientation, true, displayWidth,
                              displayHeight, widthBytes);
        }
      }
      free(table);
      free(batch);
      free(outputRow);
      free(rowBytes);
      return;
    }
    free(table);
    free(batch);

    for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
      if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
        LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
        free(outputRow);
        free(rowBytes);
        return;
      }
      const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
      const int screenY = y + bmpYOffset;
      if (screenY < 0 || screenY >= getScreenHeight()) continue;
      // BW only (1-bit images are never rendered in grayscale passes)
      bitmapFastRow<0x07>(frameBuffer, outputRow, 0, bitmap.getWidth(), x, screenY, orientation, true, displayWidth,
                          displayHeight, widthBytes);
    }
    free(outputRow);
    free(rowBytes);
    return;
  }

  // ── Downscale path: AREA-MAJORITY sampling. ──────────────────────────────────
  // The previous implementation drew a destination pixel black if ANY source pixel
  // mapping to it was black ("OR-to-black"). For a dithered 1-bit cover that scatters
  // black dots through gray regions, downscaling then made almost every destination
  // pixel catch a dot, collapsing the whole image toward black. Instead, for each
  // destination pixel count how many of the covered source pixels are black and draw
  // black only when they are the majority — i.e. resample by coverage, not by presence.
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  const int dstW = std::max(1, static_cast<int>(std::floor(srcW * scale)));

  // Per-destination-column accumulators for the destination row currently being built.
  auto* blackCount = static_cast<uint16_t*>(calloc(dstW, sizeof(uint16_t)));
  auto* totalCount = static_cast<uint16_t*>(calloc(dstW, sizeof(uint16_t)));
  if (!blackCount || !totalCount) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit downscale accumulators");
    free(blackCount);
    free(totalCount);
    free(outputRow);
    free(rowBytes);
    return;
  }

  // Flushes the accumulated destination row to the framebuffer, then clears it.
  auto flushDstRow = [&](int dstScreenY) {
    if (dstScreenY >= 0 && dstScreenY < getScreenHeight()) {
      for (int dx = 0; dx < dstW; dx++) {
        // Majority vote: black wins ties (>= half) so thin dark strokes survive.
        if (totalCount[dx] > 0 && blackCount[dx] * 2 >= totalCount[dx]) {
          const int screenX = x + dx;
          if (screenX >= 0 && screenX < getScreenWidth()) drawPixel(screenX, dstScreenY, true);
        }
      }
    }
    memset(blackCount, 0, dstW * sizeof(uint16_t));
    memset(totalCount, 0, dstW * sizeof(uint16_t));
  };

  int curDstY = -1;  // destination row currently accumulating (in screen coords)
  for (int bmpY = 0; bmpY < srcH; bmpY++) {
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      break;
    }
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : srcH - 1 - bmpY;
    const int dstScreenY = y + static_cast<int>(std::floor(bmpYOffset * scale));
    if (dstScreenY != curDstY) {
      if (curDstY != -1) flushDstRow(curDstY);
      curDstY = dstScreenY;
    }
    // Accumulate this source row's pixels into their destination columns.
    for (int bmpX = 0; bmpX < srcW; bmpX++) {
      int dx = static_cast<int>(std::floor(bmpX * scale));
      if (dx >= dstW) dx = dstW - 1;  // guard fp rounding at the right edge
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
      totalCount[dx]++;
      if (val < 3) blackCount[dx]++;  // 0,1,2 = black; 3 = white
    }
  }
  if (curDstY != -1) flushDstRow(curDstY);  // final row

  free(blackCount);
  free(totalCount);
  free(outputRow);
  free(rowBytes);
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX, nodeX + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods). Also read from other
// tasks through isComposingFrame(), hence atomic.
static std::atomic<unsigned long> start_ms{0};
static std::atomic<bool> start_ms_valid{false};

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms.store(millis(), std::memory_order_relaxed);
  start_ms_valid.store(true, std::memory_order_release);
  display.clearScreen(color);
}

bool GfxRenderer::isComposingFrame(const uint32_t maxAgeMs) const {
  if (!start_ms_valid.load(std::memory_order_acquire)) return false;
  return millis() - start_ms.load(std::memory_order_relaxed) < maxAgeMs;
}

void GfxRenderer::invertScreen() const {
  auto* p = reinterpret_cast<uint32_t*>(frameBuffer);
  const uint32_t words = frameBufferSize / 4;
  for (uint32_t i = 0; i < words; i++) p[i] = ~p[i];
  for (uint32_t i = words * 4; i < frameBufferSize; i++) frameBuffer[i] = ~frameBuffer[i];
}

void GfxRenderer::displayWindow(int logX, int logY, int logW, int logH, bool turnOffScreen) const {
  // Translate logical rectangle to physical panel coordinates using the same
  // rotation rules as rotateCoordinates(). Physical (x=source, y=gate row).
  uint16_t physX, physY, physW, physH;
  switch (getOrientation()) {
    case Portrait:
      // phyX = logY, phyY = panelHeight-1-logX (top-left corner)
      physX = static_cast<uint16_t>(logY);
      physY = static_cast<uint16_t>(panelHeight - logX - logW);
      physW = static_cast<uint16_t>(logH);
      physH = static_cast<uint16_t>(logW);
      break;
    case PortraitInverted:
      // phyX = panelWidth-1-logY, phyY = logX (top-left corner, x goes right→left)
      physX = static_cast<uint16_t>(panelWidth - logY - logH);
      physY = static_cast<uint16_t>(logX);
      physW = static_cast<uint16_t>(logH);
      physH = static_cast<uint16_t>(logW);
      break;
    case LandscapeClockwise:
      // phyX = panelWidth-1-logX, phyY = panelHeight-1-logY (180° rotation)
      physX = static_cast<uint16_t>(panelWidth - logX - logW);
      physY = static_cast<uint16_t>(panelHeight - logY - logH);
      physW = static_cast<uint16_t>(logW);
      physH = static_cast<uint16_t>(logH);
      break;
    case LandscapeCounterClockwise:
    default:
      // Native panel orientation — no transform needed
      physX = static_cast<uint16_t>(logX);
      physY = static_cast<uint16_t>(logY);
      physW = static_cast<uint16_t>(logW);
      physH = static_cast<uint16_t>(logH);
      break;
  }
  LOG_DBG("WIN", "displayWindow logical(%d,%d,%d,%d) orient=%d → physical(%d,%d,%d,%d)", logX, logY, logW, logH,
          (int)getOrientation(), physX, physY, physW, physH);
  display.displayWindow(physX, physY, physW, physH, turnOffScreen);
}

static constexpr unsigned int encodeRefreshMode(const HalDisplay::RefreshMode mode) {
  return static_cast<unsigned int>(mode) + 1u;
}

static constexpr HalDisplay::RefreshMode decodeRefreshMode(const unsigned int value) {
  return static_cast<HalDisplay::RefreshMode>(value - 1u);
}

void GfxRenderer::setNextDisplayRefreshMode(const HalDisplay::RefreshMode refreshMode) const {
  refreshOverride.store(encodeRefreshMode(refreshMode), std::memory_order_release);
}

HalDisplay::RefreshMode GfxRenderer::consumeRefreshOverride(const HalDisplay::RefreshMode requested) const {
  unsigned int overrideValue = refreshOverride.load(std::memory_order_acquire);
  if (overrideValue == REFRESH_OVERRIDE_NONE) {
    return requested;
  }
  unsigned int expected = overrideValue;
  if (refreshOverride.compare_exchange_strong(expected, REFRESH_OVERRIDE_NONE, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
    return decodeRefreshMode(overrideValue);
  }
  if (expected != REFRESH_OVERRIDE_NONE) {
    refreshOverride.store(REFRESH_OVERRIDE_NONE, std::memory_order_release);
    return decodeRefreshMode(expected);
  }
  return requested;
}

void GfxRenderer::triggerDisplay(const HalDisplay::RefreshMode mode, const bool turnOffScreen) const {
  const HalDisplay::RefreshMode effectiveMode = consumeRefreshOverride(mode);
  noteRefresh(effectiveMode);
  const bool effectiveTurnOff = turnOffScreen || fadingFix.load(std::memory_order_relaxed);
  display.triggerDisplay(effectiveMode, effectiveTurnOff);
  // triggerDisplay swaps display buffers; keep renderer's cached pointer in
  // sync so subsequent draws/grayscale passes target the active write buffer.
  frameBuffer = display.getFrameBuffer();
}

void GfxRenderer::triggerDisplayAsync(const HalDisplay::RefreshMode mode, const bool turnOffScreen) const {
  const HalDisplay::RefreshMode effectiveMode = consumeRefreshOverride(mode);
  noteRefresh(effectiveMode);
  const bool effectiveTurnOff = turnOffScreen || fadingFix.load(std::memory_order_relaxed);
  display.triggerDisplayAsync(effectiveMode, effectiveTurnOff);
  // The buffer swap happened before the waveform started; resync the cached
  // pointer now so plane renders during the waveform hit the write buffer.
  frameBuffer = display.getFrameBuffer();
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  // The web server session releases both framebuffers (releaseFrameBuffers());
  // a flush would stream the null buffer over SPI and fault (field crash: a
  // global force-refresh button pressed during a transfer). The second check
  // covers releaseFrameBuffersWithScratch, where the renderer points at a
  // scratch buffer but the display's own buffers are gone.
  if (frameBuffer == nullptr || display.getFrameBuffer() == nullptr) {
    LOG_ERR("GFX", "displayBuffer with released framebuffer ignored");
    return;
  }
  const auto effectiveMode = consumeRefreshOverride(refreshMode);
  noteRefresh(effectiveMode);

  if (start_ms_valid.load(std::memory_order_acquire)) {
    const unsigned long elapsed = millis() - start_ms.load(std::memory_order_relaxed);
    LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  } else {
    LOG_DBG("GFX", "Time = n/a from clearScreen to displayBuffer (no clearScreen marker)");
  }
  start_ms_valid.store(false, std::memory_order_release);
  display.displayBuffer(effectiveMode, fadingFix.load(std::memory_order_relaxed));
  // swapBuffers() ran inside displayBuffer(). Resync our cached frameBuffer pointer
  // from the HAL so subsequent renders (clearScreen + glyph writes) go to the correct
  // slot. Without this, GfxRenderer::frameBuffer stays stale and every other render
  // writes into the wrong buffer — cleared by display.clearScreen() but then written
  // to the old slot by glyph rendering, producing a blank page.
  frameBuffer = display.getFrameBuffer();
  // Do NOT seed RED RAM here per page. The display driver already keeps the RED
  // (previous-frame) plane current on every refresh — it writes RED from `prev` on
  // each dual-buffer fast refresh, and reseeds RED from the framebuffer after each
  // single-buffer refresh. A per-page syncRedRamFromFrameBuffer() would be a second,
  // redundant full-plane RED write. The one place the baseline genuinely needs an
  // explicit seed is the dual->single transition (before releaseSecondaryBuffer for
  // fast-diff): the release sites call syncRedRamFromFrameBuffer() there directly.
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return text;
  }

  // Scan pass: record the text and hand back the whole string. The scan has to see every glyph
  // the real pass will draw, and prewarming a truncated string would leave the remainder to the
  // per-glyph fallback. This used to fall out of getTextWidth() returning 0 while scanning, which
  // read as "fits"; now that the measurement happens inside textTruncation, say it explicitly.
  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return text;
  }

  // SMALL_CAPS scales every metric, which the single-pass walk does not model; a font with no
  // U+2026 has nothing to truncate with. Neither case occurs today, so both take the old loop.
  if (!(style & EpdFontFamily::SMALL_CAPS) && textTruncation::canTruncate(*family, style)) {
    return textTruncation::truncateToWidth(*family, text, maxWidth, style);
  }

  std::string item = text;
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) return item;
  while (!item.empty() && getTextWidth(fontId, (item + textTruncation::ELLIPSIS_UTF8).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }
  return item.empty() ? textTruncation::ELLIPSIS_UTF8 : item + textTruncation::ELLIPSIS_UTF8;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth(const Orientation orientation) const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight(const Orientation orientation) const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

int GfxRenderer::getScreenWidth() const { return getScreenWidth(getOrientation()); }

int GfxRenderer::getScreenHeight() const { return getScreenHeight(getOrientation()); }

// Inverse of rotateCoordinates: panel-native normalized touch -> logical px.
// Ported from upstream/develop so the layers above stay diff-comparable. Clamp
// first, then rotate, so an out-of-range report from the controller can never
// produce an off-screen logical point.
//
// One deliberate difference from upstream: our `orientation` member is a
// std::atomic<int>, not a plain enum, so this reads it once through
// getOrientation() instead of switching on the member directly. Reading it once
// also keeps the transform self-consistent if the reader rotates mid-call.
// The transform itself lives in TouchTransform.h so it can be host-tested
// without Arduino; keep the two orientation orders in lockstep.
static_assert(static_cast<int>(GfxRenderer::Portrait) == touchtransform::Portrait &&
                  static_cast<int>(GfxRenderer::LandscapeClockwise) == touchtransform::LandscapeClockwise &&
                  static_cast<int>(GfxRenderer::PortraitInverted) == touchtransform::PortraitInverted &&
                  static_cast<int>(GfxRenderer::LandscapeCounterClockwise) == touchtransform::LandscapeCounterClockwise,
              "touchtransform::Orientation must mirror GfxRenderer::Orientation");

void GfxRenderer::tapToLogical(const float nx, const float ny, int& outX, int& outY) const {
  // Read the atomic orientation once so the transform stays self-consistent
  // even if the reader rotates the screen mid-call.
  tapToLogical(getOrientation(), nx, ny, outX, outY);
}

void GfxRenderer::tapToLogical(const Orientation orientation, const float nx, const float ny, int& outX,
                               int& outY) const {
  touchtransform::tapToLogical(static_cast<int>(orientation), panelWidth, panelHeight, nx, ny, outX, outY);
}

static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX, phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(getOrientation(), lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1))
    return 0;
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  return static_cast<size_t>(byteX1 - byteX0 + 1) * static_cast<size_t>(y1 - y0 + 1);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(getOrientation(), lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1))
    return false;
  const int byteX0 = x0 / 8;
  const int bytesPerRow = x1 / 8 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++)
    memcpy(buf + row * bytesPerRow, frameBuffer + (y0 + row) * panelWidthBytes + byteX0, bytesPerRow);
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(getOrientation(), lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1))
    return false;
  const int byteX0 = x0 / 8;
  const int bytesPerRow = x1 / 8 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++)
    memcpy(frameBuffer + (y0 + row) * panelWidthBytes + byteX0, buf + row * bytesPerRow, bytesPerRow);
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyphRef spaceGlyph = family->getGlyph(' ', style);
  if (!spaceGlyph) return 0;
  // Scaled with the glyphs: a synthesised size laying words out at its master's inter-word
  // spacing would drift visibly across a line.
  return applyBase(fp4::toPixel(spaceGlyph.advanceX), fontBaseScale(fontId));  // 12.4 -> nearest px
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) return 0;
  const auto& font = *family;
  const EpdGlyphRef spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph.advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  // Scaled in FIXED POINT, before the snap. This is the inter-word advance ParsedText uses for
  // line breaking and justification, so it has to grow with the glyphs or a synthesised size
  // measures words at its own scale and the gaps between them at its master's -- 83% of the right
  // spacing at 24 pt, which reads as the layout struggling to fit words on a line.
  //
  // Scaling the combined fixed-point sum rather than the snapped pixel keeps the property the
  // comment above is about: one rounding step, not two.
  const float base = fontBaseScale(fontId);
  const int32_t totalFP = spaceAdvanceFP + kernFP;
  return fp4::toPixel(base == 1.0f ? totalFP : static_cast<int32_t>(totalFP * base + (totalFP < 0 ? -0.5f : 0.5f)));
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) return 0;
  const int kernFP = family->getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  // Scaled before the snap, for the same reason getSpaceAdvance() is: ParsedText adds this to
  // word widths when breaking lines, so a synthesised size kerning at its master's scale would
  // mis-measure every line.
  const float base = fontBaseScale(fontId);
  if (base != 1.0f) {
    return fp4::toPixel(static_cast<int>(kernFP * base + (kernFP < 0 ? -0.5f : 0.5f)));
  }
  return fp4::toPixel(kernFP);  // snap 4.4 fixed-point to nearest pixel
}

// Mirrors drawTextAtScale()'s cursor arithmetic step for step: scaled kern added in 12.4, scaled
// advance added in 12.4, one snap at the end. Any divergence here shows up as text that does not
// fit the box it was measured into.
int GfxRenderer::scaledTextAdvanceX(const EpdFontFamily& font, const char* text, const EpdFontFamily::Style style,
                                    const float scale) const {
  uint32_t cp;
  uint32_t prevCp = 0;
  int32_t cursorFP = 0;  // 12.4 fixed-point, exactly as the draw cursor
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) continue;
    cp = font.applyLigatures(cp, text, style);
    const bool folded = (style & EpdFontFamily::SMALL_CAPS) != 0 && smallCaps::fold(cp);
    const float effScale = folded ? scale * smallCaps::SCALE : scale;

    if (prevCp != 0) {
      const int kern = font.getKerning(prevCp, cp, style);
      cursorFP += static_cast<int>(kern * effScale + (kern < 0 ? -0.5f : 0.5f));
    }
    const EpdGlyphRef glyph = font.getGlyph(cp, style);
    if (!glyph) {
      prevCp = 0;
      continue;
    }
    cursorFP += static_cast<int>(glyph.advanceX * effScale + 0.5f);
    prevCp = cp;
  }
  return (cursorFP + 8) >> 4;  // the same snap the draw applies to a glyph position
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return 0;
  }

  // A synthesised size measures on its own path, because the two accumulation models differ and
  // this function's whole contract is that measurement and rendering agree EXACTLY.
  //
  // Unscaled, it uses differential rounding: snap (previous advance + current kern) per glyph and
  // sum pixels. drawTextAtScale() instead accumulates SCALED advances and kerns in 12.4 and snaps
  // each glyph's position out of the running cursor. Multiplying the unscaled pixel total by the
  // base would agree with neither, and ParsedText uses this for every word width when breaking
  // lines -- which is how 24 pt came to measure its words at the 20 pt master's size while drawing
  // them at 24, so the layout believed a line held more than it did.
  const float baseScale = fontBaseScale(fontId);
  if (baseScale != 1.0f) {
    return scaledTextAdvanceX(*family, text, style, baseScale);
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = *family;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);

    // Small-caps fold — mirror drawText so measurement and rendering agree exactly.
    const bool folded = (style & EpdFontFamily::SMALL_CAPS) != 0 && smallCaps::fold(cp);

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      auto kernFP = static_cast<int32_t>(font.getKerning(prevCp, cp, style));  // 4.4 fixed-point kern
      if (folded) kernFP = static_cast<int32_t>(kernFP * smallCaps::SCALE + 0.5f);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);  // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyphRef glyph = font.getGlyph(cp, style);
    if (!glyph) {
      widthPx += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      continue;
    }
    prevAdvanceFP = glyph.advanceX;
    // SUP/SUB no longer halve here — superscript/subscript words are measured at their
    // explicit per-word scale by the layout code, matching drawTextScaled exactly.
    if (folded) {
      prevAdvanceFP = static_cast<int>(prevAdvanceFP * smallCaps::SCALE + 0.5f);
    }
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::rawFontAscenderSize(const int fontId) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return family->getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  return applyBase(rawFontAscenderSize(fontId), fontBaseScale(fontId));
}

int GfxRenderer::rawLineHeight(const int fontId) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return family->getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  return applyBase(rawLineHeight(fontId), fontBaseScale(fontId));
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return applyBase(family->getData(EpdFontFamily::REGULAR)->ascender, fontBaseScale(fontId));
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const EpdFontFamily* const family = findFontFamily(fontId);
  if (!family) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  const auto& font = *family;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int lastBaseAdvanceFP = 0;  // 12.4 fixed-point
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyphRef combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph.top, combiningGlyph.height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::centerOverRotated90CW(lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph.left, combiningGlyph.width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, getRenderMode(), font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyphRef glyph = font.getGlyph(cp, style);
    if (!glyph) {
      lastBaseY -= fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      lastBaseAdvanceFP = 0;
      continue;
    }

    lastBaseLeft = glyph.left;
    lastBaseWidth = glyph.width;
    lastBaseTop = glyph.top;
    lastBaseAdvanceFP = glyph.advanceX;
    prevAdvanceFP = lastBaseAdvanceFP;

    renderCharImpl<TextRotation::Rotated90CW>(*this, getRenderMode(), font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleLsbBuffers(const uint8_t* plane) const { display.copyGrayscaleLsbBuffers(plane); }

void GfxRenderer::copyGrayscaleMsbBuffers(const uint8_t* plane) const { display.copyGrayscaleMsbBuffers(plane); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer() const { display.displayGrayBuffer(fadingFix); }

bool GfxRenderer::supportsAbsoluteGrayPlanes() const { return display.supportsAbsoluteGrayPlanes(); }

bool GfxRenderer::beginAbsoluteGrayPass(const HalDisplay::RefreshMode fallback) const {
  // Consume a pending setNextDisplayRefreshMode() override exactly as displayBuffer() and
  // triggerDisplay() do. This was the one display entry point that did not, so a reader exit's
  // enforceExitFullRefresh() HALF arrived here armed and left armed: not applied to this base,
  // and lying in wait for whatever unrelated refresh came next (device logs 2026-09-23, X3, X4
  // and X4 Pro: `Gray base: overridePending=1` on every cover sleep from the reader). The
  // override is the caller asking for a stronger base; the driver receives it as the fallback
  // mode and applies it wherever it takes a B/W base push.
  const HalDisplay::RefreshMode effective = consumeRefreshOverride(fallback);
  return display.beginAbsoluteGrayPass(effective, fadingFix);
}

uint8_t GfxRenderer::getGrayLevels() const { return display.getGrayLevels(); }

uint8_t* GfxRenderer::borrowGray8Canvas(uint16_t* stride) const { return display.borrowGray8Canvas(stride); }

void GfxRenderer::displayGray8Canvas() const { display.displayGray8Canvas(HalDisplay::FULL_REFRESH, fadingFix); }

// Bit clear = black is the framebuffer's own convention (see DirectPixelWriter's
// BW branch, which draws by clearing), and the driver expands it the same way.
void GfxRenderer::stampBwOntoGray8Canvas(uint8_t* canvas, const uint16_t stride) const {
  if (!canvas || !frameBuffer) return;
  const int width = getDisplayWidth();
  const int height = getDisplayHeight();
  const int widthBytes = getDisplayWidthBytes();
  for (int y = 0; y < height; y++) {
    const uint8_t* src = frameBuffer + static_cast<uint32_t>(y) * widthBytes;
    uint8_t* dst = canvas + static_cast<uint32_t>(y) * stride;
    for (int bx = 0; bx < widthBytes; bx++) {
      const uint8_t bits = src[bx];
      // The overwhelmingly common case on a text overlay: a fully white byte has
      // nothing to stamp, so skip the eight-bit unpack entirely.
      if (bits == 0xFF) continue;
      const int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        const int x = x0 + bit;
        if (x >= width) break;
        if (!(bits & (0x80 >> bit))) dst[x] = 0x00;
      }
    }
  }
}

void GfxRenderer::compositeBwRectOntoGray8Canvas(const Gray8Target& gray8, const int x, const int y, const int width,
                                                 const int height) const {
  if (!gray8.canvas || !frameBuffer || width <= 0 || height <= 0) return;
  const int xEnd = std::min(x + width, getScreenWidth());
  const int yEnd = std::min(y + height, getScreenHeight());
  for (int row = std::max(0, y); row < yEnd; row++) {
    for (int col = std::max(0, x); col < xEnd; col++) {
      drawGray8Pixel(gray8, col, row, getPixel(col, row) ? 0x00 : 0xFF);
    }
  }
}

bool GfxRenderer::supportsGrayFrame() const { return display.supportsGrayFrame(); }

void GfxRenderer::triggerGrayscaleFrame(const HalDisplay::RefreshMode mode) const {
  const HalDisplay::RefreshMode effectiveMode = consumeRefreshOverride(mode);
  noteRefresh(effectiveMode);
  display.triggerGrayscaleFrame(effectiveMode, fadingFix);
  // No cached-pointer resync here: unlike triggerDisplay(), this path deliberately does not swap
  // buffers (see FreeInkDisplay::displayGrayscaleFrame), so frameBuffer still points where it did.
}

void GfxRenderer::displayGrayscaleFrame(const HalDisplay::RefreshMode mode) const {
  const HalDisplay::RefreshMode effectiveMode = consumeRefreshOverride(mode);
  noteRefresh(effectiveMode);
  display.displayGrayscaleFrame(effectiveMode, fadingFix);
  // Same contract as triggerDisplay(): the display swapped buffers, so the
  // cached pointer must follow or every later draw writes to the frame now on
  // the panel.
  frameBuffer = display.getFrameBuffer();
}

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() { return storeBwBufferRect(0, 0, getScreenWidth(), getScreenHeight()); }

bool GfxRenderer::storeBwBufferRect(const int x, const int y, const int width, const int height) {
  if (width <= 0 || height <= 0) {
    freeBwBufferChunks();
    bwSnapshotRowStart = 0;
    bwSnapshotRowEnd = 0;
    bwSnapshotSizeBytes = 0;
    LOG_ERR("GFX", "!! BW buffer store rect invalid: x=%d y=%d w=%d h=%d", x, y, width, height);
    return false;
  }

  const int screenWidth = getScreenWidth();
  const int screenHeight = getScreenHeight();
  if (screenWidth <= 0 || screenHeight <= 0 || panelWidthBytes == 0 || panelHeight == 0 || !frameBuffer) {
    freeBwBufferChunks();
    bwSnapshotRowStart = 0;
    bwSnapshotRowEnd = 0;
    bwSnapshotSizeBytes = 0;
    LOG_ERR("GFX", "!! BW buffer store unavailable (screen=%dx%d panelHeight=%u rowBytes=%u fb=%p)", screenWidth,
            screenHeight, panelHeight, panelWidthBytes, frameBuffer);
    return false;
  }

  const int clampedX0 = std::max(0, x);
  const int clampedY0 = std::max(0, y);
  const int clampedX1 = std::min(screenWidth - 1, x + width - 1);
  const int clampedY1 = std::min(screenHeight - 1, y + height - 1);
  if (clampedX0 > clampedX1 || clampedY0 > clampedY1) {
    freeBwBufferChunks();
    bwSnapshotRowStart = 0;
    bwSnapshotRowEnd = 0;
    bwSnapshotSizeBytes = 0;
    LOG_ERR("GFX", "!! BW buffer store rect outside screen: x=%d y=%d w=%d h=%d", x, y, width, height);
    return false;
  }

  int rowStart = 0;
  int rowEnd = 0;
  switch (getOrientation()) {
    case LandscapeCounterClockwise:
      rowStart = clampedY0;
      rowEnd = clampedY1;
      break;
    case LandscapeClockwise:
      rowStart = static_cast<int>(panelHeight) - 1 - clampedY1;
      rowEnd = static_cast<int>(panelHeight) - 1 - clampedY0;
      break;
    case Portrait:
      rowStart = static_cast<int>(panelHeight) - 1 - clampedX1;
      rowEnd = static_cast<int>(panelHeight) - 1 - clampedX0;
      break;
    case PortraitInverted:
      rowStart = clampedX0;
      rowEnd = clampedX1;
      break;
  }

  rowStart = std::max(0, rowStart);
  rowEnd = std::min(static_cast<int>(panelHeight) - 1, rowEnd);
  if (rowStart > rowEnd) {
    freeBwBufferChunks();
    bwSnapshotRowStart = 0;
    bwSnapshotRowEnd = 0;
    bwSnapshotSizeBytes = 0;
    LOG_ERR("GFX", "!! BW buffer store row-band invalid after orientation mapping: rows=%d..%d", rowStart, rowEnd);
    return false;
  }

  const size_t rows = static_cast<size_t>(rowEnd - rowStart + 1);
  const size_t snapshotSizeBytes = rows * panelWidthBytes;
  const size_t snapshotBaseOffset = static_cast<size_t>(rowStart) * panelWidthBytes;
  if (snapshotSizeBytes == 0 || snapshotBaseOffset + snapshotSizeBytes > frameBufferSize) {
    LOG_ERR("GFX", "!! BW buffer store row-band out of bounds: base=%zu size=%zu frame=%u", snapshotBaseOffset,
            snapshotSizeBytes, frameBufferSize);
    return false;
  }

  freeBwBufferChunks();
  bwSnapshotRowStart = static_cast<uint16_t>(rowStart);
  bwSnapshotRowEnd = static_cast<uint16_t>(rowEnd);
  bwSnapshotSizeBytes = snapshotSizeBytes;

  auto attemptStore = [&](size_t chunkSize) {
    bwBufferChunks.assign((bwSnapshotSizeBytes + chunkSize - 1) / chunkSize, nullptr);
    for (size_t i = 0; i < bwBufferChunks.size(); i++) {
      if (bwBufferChunks[i]) {
        LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
        free(bwBufferChunks[i]);
        bwBufferChunks[i] = nullptr;
      }

      const size_t offset = i * chunkSize;
      const size_t allocSize = std::min(chunkSize, bwSnapshotSizeBytes - offset);
      bwBufferChunks[i] = static_cast<uint8_t*>(malloc(allocSize));

      if (!bwBufferChunks[i]) {
        const uint32_t freeHeap = esp_get_free_heap_size();
        const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
        LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes): free=%u contig=%u", i, allocSize,
                freeHeap, contigHeap);
        freeBwBufferChunks();
        return false;
      }

      memcpy(bwBufferChunks[i], frameBuffer + snapshotBaseOffset + offset, allocSize);
    }
    bwBufferChunkSize = chunkSize;
    LOG_DBG("GFX", "Stored BW buffer rows [%u..%u] (%zu bytes) in %zu chunks (%zu bytes each)", bwSnapshotRowStart,
            bwSnapshotRowEnd, bwSnapshotSizeBytes, bwBufferChunks.size(), chunkSize);
    return true;
  };

  if (attemptStore(bwBufferChunkSize)) {
    return true;
  }

  if (bwBufferChunkSize > 4096) {
    LOG_INF("GFX", "BW buffer allocation failed with chunk size %zu, retrying with 4096", bwBufferChunkSize);
    if (attemptStore(4096)) {
      return true;
    }
  }

  if (bwBufferChunkSize > 2048) {
    LOG_INF("GFX", "BW buffer allocation still failed, retrying with 2048");
    if (attemptStore(2048)) {
      return true;
    }
  }

  if (bwBufferChunkSize > 1024) {
    LOG_INF("GFX", "BW buffer allocation still failed, retrying with 1024");
    if (attemptStore(1024)) {
      return true;
    }
  }

  LOG_ERR("GFX", "!! BW buffer storage failed after retrying smaller chunk sizes");
  bwSnapshotSizeBytes = 0;
  bwSnapshotRowStart = 0;
  bwSnapshotRowEnd = 0;
  return false;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  if (bwSnapshotSizeBytes == 0) {
    display.cleanupGrayscaleBuffers(frameBuffer);
    freeBwBufferChunks();
    LOG_ERR("GFX", "BW restore skipped: no stored snapshot metadata; cleaned grayscale buffers only");
    return;
  }

  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    // Store failed part-way (or was skipped), so we cannot restore BW bytes safely.
    // Still cleanup grayscale staging buffers to avoid retaining large temporary
    // allocations that can later starve TLS handshakes.
    display.cleanupGrayscaleBuffers(frameBuffer);
    freeBwBufferChunks();
    bwSnapshotSizeBytes = 0;
    bwSnapshotRowStart = 0;
    bwSnapshotRowEnd = 0;
    LOG_ERR("GFX", "BW restore skipped due to missing chunks; cleaned grayscale buffers only");
    return;
  }

  const size_t snapshotBaseOffset = static_cast<size_t>(bwSnapshotRowStart) * panelWidthBytes;
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * bwBufferChunkSize;
    const size_t chunkSize = std::min(bwBufferChunkSize, bwSnapshotSizeBytes - offset);
    memcpy(frameBuffer + snapshotBaseOffset + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored BW buffer rows [%u..%u] (%zu bytes) and freed BW chunks", bwSnapshotRowStart,
          bwSnapshotRowEnd, bwSnapshotSizeBytes);
  bwSnapshotSizeBytes = 0;
  bwSnapshotRowStart = 0;
  bwSnapshotRowEnd = 0;
}

// Cleanup grayscale buffers using the current frame buffer.
// Use this when BW buffer was re-rendered instead of stored/restored.
// On X3 the display call transiently Y-flips frameBuffer in place and flips
// it back before returning; the logical contents are unchanged but callers
// must not race a framebuffer reader against this call. See the header.
void GfxRenderer::cleanupGrayscaleWithPreviousBuffer() const { display.cleanupGrayscaleWithPreviousBuffer(); }

void GfxRenderer::syncRedRamFromFrameBuffer() const { display.syncRedRamFromFrameBuffer(); }

void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  // Bezel overlap comes from the board profile, not from constants here.
  //
  // BoardProfile::viewableInsets exists precisely for this and documents itself
  // as "the value CrossPoint historically hardcoded for every board (tuned on the
  // X4 bezel) — override per profile as boards are measured". We were still using
  // the hardcoded copy, so every board inherited the X4's bezel geometry however
  // differently its own case sits over the glass. The profile defaults are the
  // same {9,3,3,3}, so this is behaviour-identical until a profile says otherwise.
  const BoardConfig::ViewableInsets& in = BoardConfig::ACTIVE.viewableInsets;
  const int top = in.top, right = in.right, bottom = in.bottom, left = in.left;
  switch (getOrientation()) {
    case Portrait:
      *outTop = top;
      *outRight = right;
      *outBottom = bottom;
      *outLeft = left;
      break;
    case LandscapeClockwise:
      *outTop = left;
      *outRight = top;
      *outBottom = right;
      *outLeft = bottom;
      break;
    case PortraitInverted:
      *outTop = bottom;
      *outRight = left;
      *outBottom = top;
      *outLeft = right;
      break;
    case LandscapeCounterClockwise:
      *outTop = right;
      *outRight = bottom;
      *outBottom = left;
      *outLeft = top;
      break;
  }

  // The reader's own clearance, on top of the profile's. Uniform, so it is added after the
  // rotation rather than before -- the four edges would come out the same either way, and doing
  // it here keeps the switch above a statement of the BOARD's geometry alone.
  const int pad = viewablePadding.load(std::memory_order_relaxed);
  if (pad > 0) {
    // Clamped to a quarter of each axis so a future option, or a bad settings file, cannot
    // inset the viewport out of existence.
    const int maxH = getScreenWidth() / 4;
    const int maxV = getScreenHeight() / 4;
    const int padH = pad < maxH ? pad : maxH;
    const int padV = pad < maxV ? pad : maxV;
    *outTop += padV;
    *outBottom += padV;
    *outLeft += padH;
    *outRight += padH;
  }

  // One-shot per (orientation, insets, padding) combination. The profile values are in the
  // panel's NATIVE PORTRAIT frame and are rotated above, so a change to `left`
  // does not necessarily move the screen's left edge -- in landscape it moves the
  // top. This logs both halves so the mapping can be checked against the device
  // instead of inferred.
  static int lastKey = -1;
  const int key = (static_cast<int>(getOrientation()) << 24) | (pad << 20) | (top << 16) | (right << 8) | left;
  if (key != lastKey) {
    lastKey = key;
    LOG_INF("GFX", "Viewable insets: profile T%d R%d B%d L%d +pad %d, orientation=%d -> screen T%d R%d B%d L%d", top,
            right, bottom, left, pad, static_cast<int>(getOrientation()), *outTop, *outRight, *outBottom, *outLeft);
  }
}
