#pragma once

#include <SdFat.h>

#include <cstdint>

#include "BitmapHelpers.h"

enum class BmpReaderError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,
  NotBMP,
  DIBTooSmall,
  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,
  BadDimensions,
  ImageTooLarge,
  PaletteTooLarge,
  SeekPixelDataFailed,
  BufferTooSmall,
  OomRowBuffer,
  ShortReadRow,
};

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError) { return "stub"; }

  explicit Bitmap(FsFile&, bool = false) {}
  ~Bitmap() = default;
  BmpReaderError parseHeaders() { return BmpReaderError::Ok; }
  BmpReaderError readNextRow(uint8_t*, uint8_t*) const { return BmpReaderError::Ok; }
  BmpReaderError rewindToData() const { return BmpReaderError::Ok; }
  int getWidth() const { return 0; }
  int getHeight() const { return 0; }
  bool isTopDown() const { return false; }
  bool hasGreyscale() const { return false; }
  int getRowBytes() const { return 0; }
  bool is1Bit() const { return true; }
  uint16_t getBpp() const { return 1; }
};
