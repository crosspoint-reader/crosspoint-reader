#pragma once

#include <ImageOpsConfig.h>

#if CROSSPOINT_ACCELERATED_IMAGES
#include <memory>

#include "GlyphBitmap.h"

// Private portrait packed-row sink. The frame maps source X and logical screen Y.
namespace imagePacked {
class Writer {
 public:
  Writer(const glyphBitmap::Target& target, int width, int first, int end, int top, int bottom,
         glyphBitmap::Plane plane, bool absolute, bool whiteAsTransparent, bool enabled);
  ~Writer();
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  bool isActive() const { return storage != nullptr; }
  bool writeRow(const uint8_t* row, int screenY);
  void flush();

 private:
  glyphBitmap::Target target;
  int width, first, end, top, bottom;
  glyphBitmap::Plane plane;
  bool absolute, whiteAsTransparent;
  int stride, byte = 0;
  uint8_t valid = 0;
  std::unique_ptr<uint8_t[]> storage;
};
}  // namespace imagePacked
#endif
