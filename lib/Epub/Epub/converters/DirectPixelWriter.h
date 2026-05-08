#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <stdint.h>

// Direct framebuffer writer that eliminates per-pixel overhead from the image
// rendering hot path.  Pre-computes orientation transform as linear coefficients
// and caches render-mode state so the inner loop is: one multiply, one add,
// one shift, and one AND per pixel — no branches, no method calls.
//
// Caller is responsible for ensuring (outX, outY) are within screen bounds.
// ImageBlock::render() already validates this before entering the pixel loop,
// and the JPEG/PNG callbacks pre-clamp destination ranges to screen bounds.
struct DirectPixelWriter {
  uint8_t* fb;
  GfxRenderer::RenderMode mode;
  uint16_t displayWidthBytes;  // Runtime framebuffer stride (X4: 100, X3: 99)

  // Orientation is collapsed into a linear transform:
  //   phyX = phyXBase + x * phyXStepX + y * phyXStepY
  //   phyY = phyYBase + x * phyYStepX + y * phyYStepY
  int phyXBase, phyYBase;
  int phyXStepX, phyYStepX;  // per logical-X step
  int phyXStepY, phyYStepY;  // per logical-Y step

  // Row-precomputed: the Y-dependent portion of the physical coords
  int rowPhyXBase, rowPhyYBase;

  void init(GfxRenderer& renderer) {
    fb = renderer.getFrameBuffer();
    mode = renderer.getRenderMode();
    displayWidthBytes = renderer.getDisplayWidthBytes();

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
  }

  // Call once per row before the column loop.
  // Pre-computes the Y-dependent portion so writePixel() only needs the X part.
  inline void beginRow(int logicalY) {
    rowPhyXBase = phyXBase + logicalY * phyXStepY;
    rowPhyYBase = phyYBase + logicalY * phyYStepY;
  }

  // Write a single 2-bit dithered pixel value to the framebuffer.
  // Must be called after beginRow() for the current row.
  // No bounds checking — caller guarantees coordinates are valid.
  inline void writePixel(int logicalX, uint8_t pixelValue) const {
    // Determine whether to draw based on render mode
    bool draw;
    bool state;
    switch (mode) {
      case GfxRenderer::BW:
        draw = (pixelValue < 3);
        state = true;
        break;
      case GfxRenderer::GRAYSCALE_MSB:
        draw = (pixelValue == 1 || pixelValue == 2);
        state = false;
        break;
      case GfxRenderer::GRAYSCALE_LSB:
        draw = (pixelValue == 1);
        state = false;
        break;
      default:
        return;
    }

    if (!draw) return;

    const int phyX = rowPhyXBase + logicalX * phyXStepX;
    const int phyY = rowPhyYBase + logicalX * phyYStepX;

    const uint16_t byteIndex = phyY * displayWidthBytes + (phyX >> 3);
    const uint8_t bitMask = 1 << (7 - (phyX & 7));

    if (state) {
      fb[byteIndex] &= ~bitMask;  // Clear bit (draw black)
    } else {
      fb[byteIndex] |= bitMask;  // Set bit (draw white)
    }
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
