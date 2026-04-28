#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <stdint.h>

// Direct framebuffer writer that eliminates per-pixel overhead from the image
// rendering hot path.  Pre-computes orientation transform as linear coefficients
// and caches render-mode state so the inner loop is: two increments, one shift,
// one AND, and one or two bit-writes per pixel — no multiplies, no branches on
// mode or orientation, no method calls.
//
// Usage:
//   pw.init(renderer);
//   for each row: pw.beginRow(logicalY);   // resets running X/Y state to col 0
//   for each col: pw.writePixel(value);    // advances running state, writes bit
//
// Caller must call writePixel() for every column in order (0, 1, 2, …) because
// the running state is advanced unconditionally on each call.  Caller is also
// responsible for ensuring columns are within screen bounds before entering the
// loop; no bounds checking is performed here.
struct DirectPixelWriter {
  uint8_t* fb;
  uint8_t* fb2;                // Secondary framebuffer for MSB plane (null = two-pass / not active)
  uint8_t writeFbMask;         // Bit i set → write to fb when pixelValue==i (pre-computed from render mode)
  uint8_t writeFb2Mask;        // Bit i set → write to fb2 when pixelValue==i
  bool fbClearBit;             // true = clear bit (BW black); false = set bit (all gray modes)
  uint16_t displayWidthBytes;  // Runtime framebuffer stride

  // Orientation is collapsed into a linear transform:
  //   phyX = phyXBase + x * phyXStepX + y * phyXStepY
  //   phyY = phyYBase + x * phyYStepX + y * phyYStepY
  int phyXBase, phyYBase;
  int phyXStepX, phyYStepX;  // per logical-X step
  int phyXStepY, phyYStepY;  // per logical-Y step

  // Pre-computed once in init(): physical-Y advance per logical-X step (in byte-index units).
  int32_t byteIdxYStep;

  // Running state — reset by beginRow(), advanced by writePixel().
  int curPhyX;
  int32_t curByteIdx;

  void init(const GfxRenderer& renderer) {
    fb = renderer.getFrameBuffer();
    fb2 = renderer.getSecondaryFrameBuffer();
    displayWidthBytes = renderer.getDisplayWidthBytes();

    // Pre-compute write masks once so the inner loop has zero mode branches.
    writeFbMask = 0;
    writeFb2Mask = 0;
    fbClearBit = false;
    switch (renderer.getRenderMode()) {
      case GfxRenderer::BW:
        writeFbMask = 0x3;
        fbClearBit = true;
        break;
      case GfxRenderer::GRAYSCALE_MSB:
        writeFbMask = 0x6;
        break;
      case GfxRenderer::GRAYSCALE_LSB:
        writeFbMask = 0x2;
        break;
      case GfxRenderer::GRAY2_LSB:
        writeFbMask = 0x5;
        if (fb2) writeFb2Mask = 0x3;
        break;
      case GfxRenderer::GRAY2_MSB:
        writeFbMask = 0x3;
        break;
      default:
        break;
    }

    const int phyW = renderer.getDisplayWidth();
    const int phyH = renderer.getDisplayHeight();

    switch (renderer.getOrientation()) {
      case GfxRenderer::Portrait:
        // phyX = y, phyY = (phyH-1) - x
        phyXBase = 0;
        phyYBase = phyH - 1;
        phyXStepX = 0;
        phyYStepX = -1;
        phyXStepY = 1;
        phyYStepY = 0;
        break;
      case GfxRenderer::LandscapeClockwise:
        // phyX = (phyW-1) - x, phyY = (phyH-1) - y
        phyXBase = phyW - 1;
        phyYBase = phyH - 1;
        phyXStepX = -1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = -1;
        break;
      case GfxRenderer::PortraitInverted:
        // phyX = (phyW-1) - y, phyY = x
        phyXBase = phyW - 1;
        phyYBase = 0;
        phyXStepX = 0;
        phyYStepX = 1;
        phyXStepY = -1;
        phyYStepY = 0;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        // phyX = x, phyY = y
        phyXBase = 0;
        phyYBase = 0;
        phyXStepX = 1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = 1;
        break;
      default:
        // Fallback to LandscapeCounterClockwise (identity transform)
        phyXBase = 0;
        phyYBase = 0;
        phyXStepX = 1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = 1;
        break;
    }

    // Per-column advance in physical-Y expressed as a byte-index delta.
    byteIdxYStep = static_cast<int32_t>(phyYStepX) * static_cast<int32_t>(displayWidthBytes);
  }

  // Call once per row before the column loop.
  // startLogicalX is the X coordinate of the first writePixel() call for this row (default 0).
  // Running state is initialised at startLogicalX so every subsequent writePixel() call
  // advances to startLogicalX+1, startLogicalX+2, … with zero per-pixel multiplies.
  inline void beginRow(int logicalY, int startLogicalX = 0) {
    const int rowPhyXBase = phyXBase + logicalY * phyXStepY;
    const int rowPhyYBase = phyYBase + logicalY * phyYStepY;
    curPhyX = rowPhyXBase + startLogicalX * phyXStepX;
    curByteIdx =
        static_cast<int32_t>(rowPhyYBase + startLogicalX * phyYStepX) * static_cast<int32_t>(displayWidthBytes);
  }

  // Write a single 2-bit pixel value to the framebuffer and advance to the next column.
  // Must be called after beginRow() for the current row, for every column in order.
  // No bounds checking — caller guarantees coordinates are valid.
  // No mode switch — write masks are pre-computed in init() and stored as members.
  inline void writePixel(uint8_t pixelValue) {
    const int phyX = curPhyX;
    const int32_t byteIdx = curByteIdx;
    curPhyX += phyXStepX;
    curByteIdx += byteIdxYStep;

    const bool doFb = (writeFbMask >> pixelValue) & 1;
    const bool doFb2 = (writeFb2Mask >> pixelValue) & 1;
    if (!doFb && !doFb2) return;

    const uint32_t bi = static_cast<uint32_t>(byteIdx) + static_cast<uint32_t>(phyX >> 3);
    const uint8_t bitMask = 1 << (7 - (phyX & 7));

    if (doFb) {
      if (fbClearBit)
        fb[bi] &= ~bitMask;
      else
        fb[bi] |= bitMask;
    }
    if (doFb2) fb2[bi] |= bitMask;
  }
};

// Direct cache writer that eliminates per-pixel overhead from PixelCache::setPixel().
// Pre-computes row pointer so the inner loop is just byte index + bit manipulation.
//
// Caller guarantees coordinates are within cache bounds.
struct DirectCacheWriter {
  uint8_t* buffer;
  int bytesPerRow;
  int originX;
  uint8_t* rowPtr;  // Pre-computed for current row

  void init(uint8_t* cacheBuffer, int cacheBytesPerRow, int cacheOriginX) {
    buffer = cacheBuffer;
    bytesPerRow = cacheBytesPerRow;
    originX = cacheOriginX;
    rowPtr = nullptr;
  }

  // Call once per row before the column loop.
  inline void beginRow(int screenY, int cacheOriginY) { rowPtr = buffer + (screenY - cacheOriginY) * bytesPerRow; }

  // Write a 2-bit pixel value. No bounds checking.
  inline void writePixel(int screenX, uint8_t value) const {
    const int localX = screenX - originX;
    const int byteIdx = localX >> 2;            // localX / 4
    const int bitShift = 6 - (localX & 3) * 2;  // MSB first: pixel 0 at bits 6-7
    rowPtr[byteIdx] = (rowPtr[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }
};
