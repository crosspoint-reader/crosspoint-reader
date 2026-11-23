#pragma once

#include <ImageOpsConfig.h>

#if CROSSPOINT_ACCELERATED_IMAGES
#include <GlyphBitmap.h>

#include <algorithm>

#include "DirectPixelWriter.h"

// Callers supply in-bounds portrait spans, with 1..8 rows in one physical byte.
namespace portraitImage {

inline int batchRows(const DirectPixelWriter& pixels, int y, int remaining) {
  const int physicalX = pixels.phyXBase + y * pixels.phyXStepY;
  const int untilBoundary = pixels.phyXStepY == 1 ? 8 - (physicalX & 7) : (physicalX & 7) + 1;
  return std::min(remaining, untilBoundary);
}

inline glyphBitmap::Plane planeFor(GfxRenderer::RenderMode mode) {
  // Check that the enum values match so the cast needs no runtime mapping.
  static_assert(GfxRenderer::BW == static_cast<int>(glyphBitmap::Plane::BW));
  static_assert(GfxRenderer::GRAYSCALE_LSB == static_cast<int>(glyphBitmap::Plane::GrayLSB));
  static_assert(GfxRenderer::GRAYSCALE_MSB == static_cast<int>(glyphBitmap::Plane::GrayMSB));
  return static_cast<glyphBitmap::Plane>(mode);
}

void writeLevels(DirectPixelWriter& pixels, const uint8_t* levels, size_t stride, int count, int width, int x, int y);

}  // namespace portraitImage
#endif
