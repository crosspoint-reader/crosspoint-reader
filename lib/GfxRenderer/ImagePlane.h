#pragma once

#include "GlyphBitmap.h"

namespace imagePlane {

// Source levels are 0 (black) through 3 (white). Bits outside the write mask
// retain the destination value. Callers resolve clipping and transparency.
__attribute__((always_inline)) inline void merge(uint8_t& destination, uint8_t low, uint8_t high, uint8_t mask,
                                                 glyphBitmap::Plane plane, bool absolute) {
  switch (plane) {
    case glyphBitmap::Plane::BW:
      destination &= (low & high) | static_cast<uint8_t>(~mask);
      break;
    case glyphBitmap::Plane::GrayLSB:
      if (absolute)
        destination = (destination & static_cast<uint8_t>(~mask)) | (low & mask);
      else
        destination |= low & static_cast<uint8_t>(~high) & mask;
      break;
    case glyphBitmap::Plane::GrayMSB:
      if (absolute)
        destination = (destination & static_cast<uint8_t>(~mask)) | (high & mask);
      else
        destination |= (low ^ high) & mask;
      break;
  }
}

}  // namespace imagePlane
