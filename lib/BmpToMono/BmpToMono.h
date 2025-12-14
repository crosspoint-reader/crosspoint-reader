#pragma once

#include <Arduino.h>
#include <FS.h>

struct MonoBitmap {
  int width = 0;
  int height = 0;
  size_t len = 0;           // bytesPerRow * height
  uint8_t* data = nullptr;  // row-aligned, MSB-first, 1=white 0=black
};

enum class BmpToMonoError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,

  NotBMP,
  DIBTooSmall,

  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,

  BadDimensions,

  SeekPixelDataFailed,
  OomOutput,
  OomRowBuffer,
  ShortReadRow,
};

class BmpToMono {
 public:
  // No rotation: output size == BMP size
  static BmpToMonoError convert24(fs::File& file, MonoBitmap& out, uint8_t threshold = 160, bool invert = false);

  // Rotate 90° clockwise: (w,h) -> (h,w)
  // Useful for converting portrait BMP (480x800) into landscape framebuffer (800x480).
  static BmpToMonoError convert24Rotate90CW(fs::File& file, MonoBitmap& out, uint8_t threshold = 160,
                                            bool invert = false);

  static void freeMonoBitmap(MonoBitmap& bmp);
  static const char* errorToString(BmpToMonoError err);

 private:
  static uint16_t readLE16(fs::File& f);
  static uint32_t readLE32(fs::File& f);

  // Writes a single pixel into a row-aligned 1bpp buffer (MSB-first), 0=black, 1=white
  static inline void setMonoPixel(uint8_t* buf, int w, int x, int y, bool isBlack) {
    const size_t bytesPerRow = (size_t)(w + 7) / 8;
    const size_t idx = (size_t)y * bytesPerRow + (size_t)(x >> 3);
    const uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    if (isBlack)
      buf[idx] &= (uint8_t)~mask;
    else
      buf[idx] |= mask;
  }

  static BmpToMonoError convert24Impl(fs::File& file, MonoBitmap& out, uint8_t threshold, bool invert, bool rotate90CW);
};