#include "GfxRenderer.h"

#include <Logging.h>
#include <Utf8.h>

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    if (!fontDecompressor) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint16_t glyphIndex = static_cast<uint16_t>(glyph - fontData->glyph);
    return fontDecompressor->getBitmap(fontData, glyph, glyphIndex);
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, font}); }

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - x;
      *phyY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = HalDisplay::DISPLAY_WIDTH - 1 - y;
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

// Write up to 8 foreground bits into a physical framebuffer row.
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

static void renderGlyphFastBW(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                              const int glyphHeight, const int screenXBase, const int screenYBase,
                              const bool pixelState, const GfxRenderer::Orientation orientation) {
  switch (orientation) {
    case GfxRenderer::LandscapeCounterClockwise: {
      // phyX = screenXBase+glyphX,  phyY = screenYBase+glyphY  (identity mapping)
      // Each glyph row is a contiguous physical h-span — read and write 8 px at a time.
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
      // phyX = W-1-screenXBase-glyphX,  phyY = H-1-screenYBase-glyphY  (180° flip)
      // glyphX=0 is rightmost; iterate glyph row right-to-left in 8-px chunks so each
      // chunk writes a contiguous left-to-right physical h-span after bit-reversal.
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - (screenYBase + glyphY);
        if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
        uint8_t* const row = frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES;
        const int rowBitStart = glyphY * glyphWidth;
        for (int chunkEnd = glyphWidth - 1; chunkEnd >= 0; chunkEnd -= 8) {
          const int chunkStart = std::max(0, chunkEnd - 7);
          const int count = chunkEnd - chunkStart + 1;
          // Read chunk in glyph (left-to-right) order then reverse bits so MSB maps to
          // glyphX=chunkEnd, which is the leftmost physical pixel of this chunk.
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
      // phyX = screenYBase+glyphY,  phyY = H-1-screenXBase-glyphX  (90° CW)
      // A glyph column maps to a physical row.  Process in 8-row × 8-col blocks:
      //   pack 8 glyph rows (sequential reads) into uint64_t → transpose8x8 →
      //   each output byte is one glyph column's bits, MSB = row 0 = smallest phyX.
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = screenYBase + glyphY;  // leftmost phyX of this row-chunk
        if (phyBitPos + rowCount <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          uint64_t pack = 0;
          int bitStart = glyphY * glyphWidth + glyphX;
          for (int n = 0; n < rowCount; n++, bitStart += glyphWidth) {
            pack |= static_cast<uint64_t>(bitmapExtract(bitmap, bitStart, colCount)) << (56 - 8 * n);
          }
          pack = transpose8x8(pack);
          // Byte k of pack = column (glyphX+k) bits, MSB = row 0 = leftmost phyX.
          for (int k = 0; k < colCount; k++) {
            const uint8_t cols_k = static_cast<uint8_t>(pack >> (56 - 8 * k));
            if (cols_k == 0) continue;
            const int phyY = HalDisplay::DISPLAY_HEIGHT - 1 - (screenXBase + glyphX + k);
            if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
            writeRowBits(frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES, phyBitPos, cols_k, pixelState);
          }
        }
      }
      break;
    }

    case GfxRenderer::PortraitInverted: {
      // phyX = W-1-screenYBase-glyphY,  phyY = screenXBase+glyphX  (90° CCW)
      // Like Portrait but glyphY=0 is the rightmost physical pixel.  Pack rows in
      // reverse order (last row at uint64_t MSB) so the transposed column bytes already
      // have MSB = last row = leftmost phyX — no bit-reversal step needed.
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        // Leftmost phyX = W-1-screenYBase-(glyphY+rowCount-1).
        const int phyBitPos = HalDisplay::DISPLAY_WIDTH - 1 - screenYBase - (glyphY + rowCount - 1);
        if (phyBitPos + rowCount <= 0 || phyBitPos >= HalDisplay::DISPLAY_WIDTH) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          // Pack row (rowCount-1) at MSB down to row 0 at the lowest active byte.
          uint64_t pack = 0;
          int bitStart = glyphY * glyphWidth + glyphX;
          for (int n = 0; n < rowCount; n++, bitStart += glyphWidth) {
            pack |= static_cast<uint64_t>(bitmapExtract(bitmap, bitStart, colCount)) << (56 - 8 * (rowCount - 1 - n));
          }
          pack = transpose8x8(pack);
          // Byte k = column (glyphX+k) bits, MSB = last row = leftmost phyX.
          for (int k = 0; k < colCount; k++) {
            const uint8_t cols_k = static_cast<uint8_t>(pack >> (56 - 8 * k));
            if (cols_k == 0) continue;
            const int phyY = screenXBase + glyphX + k;
            if (phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
            writeRowBits(frameBuffer + phyY * HalDisplay::DISPLAY_WIDTH_BYTES, phyBitPos, cols_k, pixelState);
          }
        }
      }
      break;
    }
  }
}

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int* cursorX, int* cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    glyph = fontFamily.getGlyph(REPLACEMENT_GLYPH, style);
  }

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
      outerBase = *cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = *cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = *cursorY - top;   // screenY = outerBase + glyphY
      innerBase = *cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
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
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            renderer.drawPixel(screenX, screenY, false);
          } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            renderer.drawPixel(screenX, screenY, false);
          }
        }
      }
    } else {
      // Fast path: 1-bit BW mode, non-rotated text — byte-level framebuffer writes, no drawPixel() per pixel.
      if constexpr (rotation == TextRotation::None) {
        if (renderMode == GfxRenderer::BW) {
          renderGlyphFastBW(renderer.getFrameBuffer(), bitmap, width, height, innerBase, outerBase, pixelState,
                            renderer.getOrientation());
          *cursorX += glyph->advanceX;
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

  if constexpr (rotation == TextRotation::Rotated90CW) {
    *cursorY -= glyph->advanceX;
  } else {
    *cursorX += glyph->advanceX;
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY);

  // Bounds checking against physical panel dimensions
  if (phyX < 0 || phyX >= HalDisplay::DISPLAY_WIDTH || phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Calculate byte position and bit position
  const uint16_t byteIndex = phyY * HalDisplay::DISPLAY_WIDTH_BYTES + (phyX / 8);
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
  int yPos = y + getFontAscenderSize(fontId);
  int xpos = x;

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderChar(font, cp, &xpos, &yPos, black, style);
  }
}

#ifdef ENABLE_RENDERCHAR_BENCHMARK
// Legacy per-pixel rendering path — mirrors the old renderCharImpl 1-bit BW loop.
// Used only by the renderChar benchmark to establish the baseline.
void GfxRenderer::drawTextBWLegacy(const int fontId, const int x, const int y, const char* text) const {
  if (text == nullptr || *text == '\0') return;
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return;
  const auto& fontFamily = fontIt->second;

  int yPos = y + getFontAscenderSize(fontId);
  int xPos = x;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    const EpdGlyph* glyph = fontFamily.getGlyph(cp, EpdFontFamily::REGULAR);
    if (!glyph) glyph = fontFamily.getGlyph(REPLACEMENT_GLYPH, EpdFontFamily::REGULAR);
    if (!glyph) continue;
    const EpdFontData* fontData = fontFamily.getData(EpdFontFamily::REGULAR);
    if (fontData->is2Bit) {
      xPos += glyph->advanceX;
      continue;
    }
    const uint8_t* bitmap = getGlyphBitmap(fontData, glyph);
    if (bitmap != nullptr) {
      const int screenYBase = yPos - glyph->top;
      const int screenXBase = xPos + glyph->left;
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < glyph->height; glyphY++) {
        for (int glyphX = 0; glyphX < glyph->width; glyphX++, pixelPosition++) {
          const uint8_t bit = (bitmap[pixelPosition >> 3] >> (7 - (pixelPosition & 7))) & 1;
          if (!bit) continue;
          // Inline drawPixel without OOB logging — mirrors the old per-pixel path but clips silently,
          // matching the fast path's behaviour so the benchmark measures rendering cost only.
          int phyX, phyY;
          rotateCoordinates(orientation, screenXBase + glyphX, screenYBase + glyphY, &phyX, &phyY);
          if (phyX < 0 || phyX >= HalDisplay::DISPLAY_WIDTH || phyY < 0 || phyY >= HalDisplay::DISPLAY_HEIGHT) continue;
          const uint16_t byteIndex = phyY * HalDisplay::DISPLAY_WIDTH_BYTES + (phyX / 8);
          const uint8_t bitPosition = 7 - (phyX % 8);
          frameBuffer[byteIndex] &= ~(1 << bitPosition);  // black pixel
        }
      }
    }
    xPos += glyph->advanceX;
  }
}
#endif  // ENABLE_RENDERCHAR_BENCHMARK

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
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
  const int outerRadiusSq = maxRadius * maxRadius;
  const int innerRadiusSq = innerRadius * innerRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      if (distSq > outerRadiusSq || distSq < innerRadiusSq) {
        continue;
      }
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      drawPixel(px, py, state);
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

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
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
  } else if (color == Color::LightGray) {
    for (int fillY = y; fillY < y + height; fillY++) {
      for (int fillX = x; fillX < x + width; fillX++) {
        drawPixelDither<Color::LightGray>(fillX, fillY);
      }
    }
  } else if (color == Color::DarkGray) {
    for (int fillY = y; fillY < y + height; fillY++) {
      for (int fillX = x; fillX < x + width; fillX++) {
        drawPixelDither<Color::DarkGray>(fillX, fillY);
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  const int radiusSq = maxRadius * maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      if (distSq <= radiusSq) {
        drawPixelDither<color>(px, py);
      }
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
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY);
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

  if (maxWidth > 0 && (1.0f - cropX) * bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>((1.0f - cropX) * bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && (1.0f - cropY) * bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>((1.0f - cropY) * bitmap.getHeight()));
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
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
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
  for (int i = 0; i < HalDisplay::BUFFER_SIZE; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(refreshMode, fadingFix);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  const char* ellipsis = "...";
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

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return HalDisplay::DISPLAY_HEIGHT;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return HalDisplay::DISPLAY_WIDTH;
  }
  return HalDisplay::DISPLAY_HEIGHT;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return HalDisplay::DISPLAY_WIDTH;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return HalDisplay::DISPLAY_HEIGHT;
  }
  return HalDisplay::DISPLAY_WIDTH;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? spaceGlyph->advanceX : 0;
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  int width = 0;
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) glyph = font.getGlyph(REPLACEMENT_GLYPH, style);
    if (glyph) width += glyph->advanceX;
  }
  return width;
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

  int xPos = x;
  int yPos = y;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, &xPos, &yPos, black, style);
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() { return HalDisplay::BUFFER_SIZE; }

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
  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(BW_BUFFER_CHUNK_SIZE));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, BW_BUFFER_CHUNK_SIZE);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, BW_BUFFER_CHUNK_SIZE);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", BW_BUFFER_NUM_CHUNKS, BW_BUFFER_CHUNK_SIZE);
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
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    memcpy(frameBuffer + offset, bwBufferChunks[i], BW_BUFFER_CHUNK_SIZE);
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

void GfxRenderer::renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                             EpdFontFamily::Style style) const {
  renderCharImpl<TextRotation::None>(*this, renderMode, fontFamily, cp, x, y, pixelState, style);
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
