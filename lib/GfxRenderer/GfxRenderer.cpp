#include "GfxRenderer.h"

#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Utf8.h>

#include "FontCacheManager.h"

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  return &fontData->bitmap[glyph->dataOffset];
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
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, font}); }

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
//   screenXBase = cursorX + glyph->left  (logical X of glyph pixel [0,0])
//   screenYBase = cursorY - glyph->top   (logical Y of glyph pixel [0,0])

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
static inline void writeRowBits(uint8_t* const row, const int phyBitPos, const uint8_t bits, const bool pixelState) {
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
    if (shift > 0 && byteIdx + 1 < HalDisplay::DISPLAY_WIDTH_BYTES)
      row[byteIdx + 1] &= ~(uint8_t)(effectiveBits << (8 - shift));
  } else {
    row[byteIdx] |= (effectiveBits >> shift);
    if (shift > 0 && byteIdx + 1 < HalDisplay::DISPLAY_WIDTH_BYTES)
      row[byteIdx + 1] |= (uint8_t)(effectiveBits << (8 - shift));
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
                                             const bool pixelState) {
  for (int k = 0; k < colCount; k++) {
    const uint8_t cols_k = static_cast<uint8_t>(pack >> (56 - 8 * k));
    if (cols_k == 0) continue;
    const int phyY = phyYBase + k * phyYStride;
    if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
    writeRowBits(frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES, phyBitPos, cols_k, pixelState);
  }
}

static void renderGlyphFastBW(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                              const int glyphHeight, const int screenXBase, const int screenYBase,
                              const bool pixelState, const GfxRenderer::Orientation orientation) {
  switch (orientation) {
    case GfxRenderer::LandscapeCounterClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = screenYBase + glyphY;
        if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
        uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
        const int rowBitStart = glyphY * glyphWidth;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int count = std::min(8, glyphWidth - glyphX);
          const uint8_t gbyte = bitmapExtract(bitmap, rowBitStart + glyphX, count);
          if (gbyte == 0) continue;
          const int phyBitPos = screenXBase + glyphX;
          if (phyBitPos + count <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
          writeRowBits(row, phyBitPos, gbyte, pixelState);
        }
      }
      break;
    }

    case GfxRenderer::LandscapeClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - (screenYBase + glyphY);
        if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
        uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
        const int rowBitStart = glyphY * glyphWidth;
        for (int chunkEnd = glyphWidth - 1; chunkEnd >= 0; chunkEnd -= 8) {
          const int chunkStart = std::max(0, chunkEnd - 7);
          const int count = chunkEnd - chunkStart + 1;
          const uint8_t gbyte_fwd = bitmapExtract(bitmap, rowBitStart + chunkStart, count);
          const uint8_t gbyte = reverseBits8(gbyte_fwd >> (8 - count));
          if (gbyte == 0) continue;
          const int phyBitPos = HalDisplay::DISPLAY_WIDTH - 1 - screenXBase - chunkEnd;
          if (phyBitPos + count <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
          writeRowBits(row, phyBitPos, gbyte, pixelState);
        }
      }
      break;
    }

    case GfxRenderer::Portrait: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = screenYBase + glyphY;
        if (phyBitPos + rowCount <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          const uint64_t pack =
              transpose8x8(extractGlyphBlock(bitmap, glyphWidth, glyphX, glyphY, rowCount, colCount, false));
          scatterBlockToFrameBuffer(frameBuffer, pack, colCount, HalDisplay::DISPLAY_HEIGHT - 1 - screenXBase - glyphX,
                                    -1, phyBitPos, pixelState);
        }
      }
      break;
    }

    case GfxRenderer::PortraitInverted: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = HalDisplay::DISPLAY_WIDTH - 1 - screenYBase - (glyphY + rowCount - 1);
        if (phyBitPos + rowCount <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          const uint64_t pack =
              transpose8x8(extractGlyphBlock(bitmap, glyphWidth, glyphX, glyphY, rowCount, colCount, true));
          scatterBlockToFrameBuffer(frameBuffer, pack, colCount, screenXBase + glyphX, 1, phyBitPos, pixelState);
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
// ───────────────────────────────────────────────────────────────────────────
static inline uint8_t drawMaskFor2BitMode(const GfxRenderer::RenderMode mode, const uint8_t darkness) {
  if (mode == GfxRenderer::BW) return 0x0E;  // draw raw {1,2,3}
  if (darkness >= 3) return 0x00;            // skip grayscale entirely (Maximum)
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
  if constexpr (drawMask == 0x0E) {  // BW: raw ∈ {1,2,3}
    draw0 = msb0 | lsb0;
    draw1 = msb1 | lsb1;
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
template <uint8_t drawMask, bool inverted>
static void renderGlyphFast2BitPortrait(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                                        const int glyphHeight, const int screenXBase, const int screenYBase,
                                        const bool writeState) {
  for (int glyphX = 0; glyphX < glyphWidth; glyphX++) {
    const int phyY = inverted ? (screenXBase + glyphX) : (HalDisplay::DISPLAY_HEIGHT - 1 - (screenXBase + glyphX));
    if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
    uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
    for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
      const int count = std::min(8, glyphHeight - glyphY);
      const uint8_t mask = build2BitColMask<drawMask>(bitmap, glyphWidth, glyphX, glyphY, count, inverted);
      if (mask == 0) continue;
      const int phyBitPos =
          inverted ? (HalDisplay::DISPLAY_WIDTH - 1 - screenYBase - (glyphY + count - 1)) : (screenYBase + glyphY);
      if (phyBitPos + count <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
      writeRowBits(row, phyBitPos, mask, writeState);
    }
  }
}

template <uint8_t drawMask>
static void renderGlyphFast2Bit(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                                const int glyphHeight, const int screenXBase, const int screenYBase,
                                const bool pixelState, const GfxRenderer::Orientation orientation) {
  // Non-rotated text fast path for 2-bit glyphs. Writes compact masks directly to framebuffer rows.
  // TextRotation::Rotated90CW keeps the legacy per-pixel fallback path for safety and readability.
  // BW (drawMask 0x0E) honors the caller's pixelState; grayscale passes always clear the bit.
  const bool writeState = (drawMask == 0x0E) ? pixelState : false;

  switch (orientation) {
    case GfxRenderer::LandscapeCounterClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = screenYBase + glyphY;
        if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
        uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
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
          if (phyBitPos + count <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
          writeRowBits(row, phyBitPos, mask, writeState);
        }
      }
      break;
    }

    case GfxRenderer::LandscapeClockwise: {
      // Row-outer/chunk-inner: framebuffer rows are written at stride -DISPLAY_WIDTH_BYTES
      // (phyY decreases as glyphY increases). Keeping row-outer preserves sequential access
      // within each row, which is more cache-friendly than the chunk-outer alternative.
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - (screenYBase + glyphY);
        if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
        uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
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
          const int phyBitPos = HalDisplay::DISPLAY_WIDTH - 1 - screenXBase - chunkEnd;
          if (phyBitPos + count <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
          writeRowBits(row, phyBitPos, mask, writeState);
        }
      }
      break;
    }

    case GfxRenderer::Portrait:
      renderGlyphFast2BitPortrait<drawMask, false>(frameBuffer, bitmap, glyphWidth, glyphHeight, screenXBase,
                                                   screenYBase, writeState);
      break;

    case GfxRenderer::PortraitInverted:
      renderGlyphFast2BitPortrait<drawMask, true>(frameBuffer, bitmap, glyphWidth, glyphHeight, screenXBase,
                                                  screenYBase, writeState);
      break;
  }
}

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

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
        switch (drawMask) {
          case 0x0E:  // BW
            renderGlyphFast2Bit<0x0E>(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase,
                                      pixelState, renderer.getOrientation());
            break;
          case 0x06:  // raw {1,2}
            renderGlyphFast2Bit<0x06>(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase,
                                      pixelState, renderer.getOrientation());
            break;
          case 0x04:  // raw {2}
            renderGlyphFast2Bit<0x04>(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase,
                                      pixelState, renderer.getOrientation());
            break;
          case 0x02:  // raw {1}
            renderGlyphFast2Bit<0x02>(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase,
                                      pixelState, renderer.getOrientation());
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
      if constexpr (rotation == TextRotation::None) {
        if (renderMode == GfxRenderer::BW) {
          renderGlyphFastBW(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase, pixelState,
                            renderer.getOrientation());
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

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
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

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, combiningGlyph->left,
                                                       combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
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

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    lastBaseAdvanceFP = glyph->advanceX;
    prevAdvanceFP = lastBaseAdvanceFP;

    renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    prevCp = cp;
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    // In Portrait/PortraitInverted a logical vertical line maps to a physical horizontal span.
    switch (orientation) {
      case Portrait:
        fillPhysicalHSpan(HalDisplay::DISPLAY_HEIGHT - 1 - x1, y1, y2, state);
        return;
      case PortraitInverted:
        fillPhysicalHSpan(x1, HalDisplay::DISPLAY_WIDTH - 1 - y2, HalDisplay::DISPLAY_WIDTH - 1 - y1, state);
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
    switch (orientation) {
      case LandscapeCounterClockwise:
        fillPhysicalHSpan(y1, x1, x2, state);
        return;
      case LandscapeClockwise:
        fillPhysicalHSpan(HalDisplay::DISPLAY_HEIGHT - 1 - y1, HalDisplay::DISPLAY_WIDTH - 1 - x2,
                          HalDisplay::DISPLAY_WIDTH - 1 - x1, state);
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
  const int cX1 = std::min(phyX_end, (int)HalDisplay::DISPLAY_WIDTH - 1);
  if (cX0 > cX1 || phyY < 0 || phyY >= (int)HalDisplay::DISPLAY_HEIGHT) return;

  uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
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

  // For each orientation, one logical dimension maps to a constant physical row, allowing the
  // perpendicular dimension to be written as a byte-level span — eliminating per-pixel overhead.
  switch (orientation) {
    case Portrait:
      // Logical column x → physical row (479-x); logical y range → physical x span
      for (int lx = x; lx < x + width; lx++) {
        fillPhysicalHSpan(HalDisplay::DISPLAY_HEIGHT - 1 - lx, y, y + height - 1, state);
      }
      return;
    case PortraitInverted:
      // Logical column x → physical row x; logical y range → physical x span (mirrored)
      for (int lx = x; lx < x + width; lx++) {
        fillPhysicalHSpan(lx, HalDisplay::DISPLAY_WIDTH - 1 - (y + height - 1), HalDisplay::DISPLAY_WIDTH - 1 - y,
                          state);
      }
      return;
    case LandscapeCounterClockwise:
      // Logical row y → physical row y; logical x range → physical x span
      for (int ly = y; ly < y + height; ly++) {
        fillPhysicalHSpan(ly, x, x + width - 1, state);
      }
      return;
    case LandscapeClockwise:
      // Logical row y → physical row (479-y); logical x range → physical x span (mirrored)
      for (int ly = y; ly < y + height; ly++) {
        fillPhysicalHSpan(HalDisplay::DISPLAY_HEIGHT - 1 - ly, HalDisplay::DISPLAY_WIDTH - 1 - (x + width - 1),
                          HalDisplay::DISPLAY_WIDTH - 1 - x, state);
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
    switch (orientation) {
      case Portrait:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - lx;
          const uint8_t pb = (phyY % 2 == 0) ? 0xAA : 0x55;
          fillPhysicalHSpanByte(phyY, y, y + height - 1, pb);
        }
        return;
      case PortraitInverted:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = lx;
          const uint8_t pb = (phyY % 2 == 0) ? 0xAA : 0x55;
          fillPhysicalHSpanByte(phyY, HalDisplay::DISPLAY_WIDTH - 1 - (y + height - 1),
                                HalDisplay::DISPLAY_WIDTH - 1 - y, pb);
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
          const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - ly;
          const uint8_t pb = (phyY % 2 == 0) ? 0x55 : 0xAA;
          fillPhysicalHSpanByte(phyY, HalDisplay::DISPLAY_WIDTH - 1 - (x + width - 1),
                                HalDisplay::DISPLAY_WIDTH - 1 - x, pb);
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
    switch (orientation) {
      case Portrait:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - lx;
          if (phyY % 2 == 0) continue;  // all-white row — no dark pixels to write
          fillPhysicalHSpanByte(phyY, y, y + height - 1, 0x55);
        }
        return;
      case PortraitInverted:
        for (int lx = x; lx < x + width; lx++) {
          const int phyY = lx;
          if (phyY % 2 != 0) continue;  // all-white row
          fillPhysicalHSpanByte(phyY, HalDisplay::DISPLAY_WIDTH - 1 - (y + height - 1),
                                HalDisplay::DISPLAY_WIDTH - 1 - y, 0xAA);
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
          const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - ly;
          if (phyY % 2 == 0) continue;  // all-white row
          fillPhysicalHSpanByte(phyY, HalDisplay::DISPLAY_WIDTH - 1 - (x + width - 1),
                                HalDisplay::DISPLAY_WIDTH - 1 - x, 0xAA);
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
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
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

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
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
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

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
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

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

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
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

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || (gpio.deviceIsX4() && val == 2))) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  free(outputRow);
  free(rowBytes);
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

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

    // Sort nodes by X (simple bubble sort, numPoints is small)
    for (int i = 0; i < nodes - 1; i++) {
      for (int k = i + 1; k < nodes; k++) {
        if (nodeX[i] > nodeX[k]) {
          int temp = nodeX[i];
          nodeX[i] = nodeX[k];
          nodeX[k] = temp;
        }
      }
    }

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

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  display.clearScreen(color);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::setNextDisplayRefreshMode(const HalDisplay::RefreshMode refreshMode) const {
  useNextRefreshOverride = true;
  nextRefreshOverride = refreshMode;
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  const auto effectiveMode = useNextRefreshOverride ? nextRefreshOverride : refreshMode;
  useNextRefreshOverride = false;
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(effectiveMode, fadingFix);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
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
int GfxRenderer::getScreenWidth() const {
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

int GfxRenderer::getScreenHeight() const {
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

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);                                           // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);         // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) {
      widthPx += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      continue;
    }
    prevAdvanceFP = glyph->advanceX;
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }

  const auto& font = fontIt->second;

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
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::centerOverRotated90CW(lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
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

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    lastBaseAdvanceFP = glyph->advanceX;
    prevAdvanceFP = lastBaseAdvanceFP;

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer() const { display.displayGrayBuffer(fadingFix); }

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
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
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
    LOG_ERR("GFX", "BW restore skipped due to missing chunks; cleaned grayscale buffers only");
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
