#pragma once

#include <ImageOpsConfig.h>

#if CROSSPOINT_ACCELERATED_IMAGES
#include <ImageOps.h>

#include <algorithm>
#include <cstddef>

#include "ImagePlane.h"

namespace imagePacked {

// Rows are indexed by bit lane in one physical framebuffer byte. Null lanes are untouched.
using BandRows = const uint8_t* [8];

struct BandColumns {
  int width;  // Full source width; 0 <= first < end <= width.
  int first;
  int end;
};

struct BandTarget {
  uint8_t* firstByte;         // Destination of the first clipped source column
  std::ptrdiff_t columnStep;  // Signed framebuffer stride
  glyphBitmap::Plane plane;
  bool absolute;
};

// Draw applies the normal plane rules; it does not overwrite BW pixels with white.
enum class WhitePixels { Draw, KeepBackground };

namespace detail {

// Draw immediately. Valid lanes provide ceil(width / 4) MSB-first packed bytes.
// The caller owns clipping, batching, and storage.
template <typename RowAtLane>
inline void drawBand(RowAtLane rowAtLane, uint8_t valid, BandColumns columns, BandTarget target,
                     WhitePixels whitePixels) {
  constexpr uint8_t WHITE[2] = {0xff, 0xff};
  for (int column = columns.first & ~7; column < columns.end; column += 8) {
    const uint8_t* tileRows[8];
    uint8_t tail[8][2];
    alignas(16) uint8_t planes[16];
    for (int lane = 0; lane < 8; ++lane) {
      if (!(valid & (0x80u >> lane))) {
        tileRows[lane] = WHITE;
      } else {
        const auto* row = rowAtLane(lane) + column / 4;
        if (column + 4 < columns.width) {
          tileRows[lane] = row;
        } else {
          // The tile converter consumes two bytes even for a one-byte tail.
          tail[lane][0] = *row;
          tail[lane][1] = 0xff;
          tileRows[lane] = tail[lane];
        }
      }
    }
    imageops::packed2BitTile8x8ToPlanes(tileRows, planes);
    const int first = std::max(columns.first - column, 0);
    const int last = std::min(8, columns.end - column);
    for (int c = first; c < last; ++c) {
      const uint8_t low = planes[c], high = planes[8 + c];
      const uint8_t mask =
          whitePixels == WhitePixels::KeepBackground ? valid & static_cast<uint8_t>(~(low & high)) : valid;
      const auto offset = (column + c - columns.first) * target.columnStep;
      imagePlane::merge(target.firstByte[offset], low, high, mask, target.plane, target.absolute);
    }
  }
}

}  // namespace detail

inline void drawBand(const BandRows& rows, BandColumns columns, BandTarget target, WhitePixels whitePixels) {
  uint8_t valid = 0;
  for (int lane = 0; lane < 8; ++lane) {
    if (rows[lane]) valid |= 0x80u >> lane;
  }
  detail::drawBand([&rows](int lane) { return rows[lane]; }, valid, columns, target, whitePixels);
}

inline void drawStridedBand(const uint8_t* rows, int stride, uint8_t valid, BandColumns columns, BandTarget target,
                            WhitePixels whitePixels) {
  detail::drawBand([rows, stride](int lane) { return rows + lane * stride; }, valid, columns, target, whitePixels);
}

}  // namespace imagePacked
#endif
