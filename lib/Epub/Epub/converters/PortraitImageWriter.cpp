#include "PortraitImageWriter.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <ImageOps.h>
#include <ImagePlane.h>

#include <cstring>

namespace portraitImage {
void writeLevels(DirectPixelWriter& pixels, const uint8_t* levels, size_t stride, int count, int width, int x, int y) {
  pixels.beginRow(y);
  int start, end;
  pixels.bandColRange(x, width, start, end);
  if (start >= end) return;
  const int phase = pixels.rowPhyXBase & 7;
  const int byte = pixels.rowPhyXBase / 8;
  uint8_t valid = 0;
  for (int row = 0; row < count; ++row) valid |= 0x80 >> (phase + row * pixels.phyXStepY);
  for (int column = start; column < end; column += 8) {
    alignas(16) uint8_t tile[64];
    alignas(16) uint8_t planes[16];
    memset(tile, 3, sizeof(tile));
    const int columns = std::min(8, end - column);
    for (int row = 0; row < count; ++row) {
      const int lane = phase + row * pixels.phyXStepY;
      memcpy(tile + lane * 8, levels + row * stride + column, columns);
    }
    imageops::levelTile8x8ToPlanes(tile, planes);
    for (int c = 0; c < columns; ++c) {
      const int localY = pixels.phyYBase + (x + column + c) * pixels.phyYStepX - pixels.originY;
      imagePlane::merge(pixels.fb[localY * pixels.displayWidthBytes + byte], planes[c], planes[8 + c], valid,
                        planeFor(pixels.mode), pixels.absolute);
    }
  }
}
}  // namespace portraitImage
#endif
