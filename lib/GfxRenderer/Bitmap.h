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
  static const char *errorToString(BmpReaderError err);

  explicit Bitmap(FsFile &file, bool dithering = false)
      : file(&file), dithering(dithering) {}
  explicit Bitmap(const uint8_t *buffer, size_t size, bool dithering = false)
      : file(nullptr), memoryBuffer(buffer), memorySize(size),
        dithering(dithering) {}

  ~Bitmap();
  BmpReaderError parseHeaders();
  BmpReaderError readNextRow(uint8_t *data, uint8_t *rowBuffer) const;
  BmpReaderError rewindToData() const;

  // Getters
  int getWidth() const { return width; }
  int getHeight() const { return height; }
  bool isTopDown() const { return topDown; }
  bool hasGreyscale() const { return bpp > 1; }
  int getRowBytes() const { return rowBytes; }
  bool is1Bit() const { return bpp == 1; }
  uint16_t getBpp() const { return bpp; }

private:
  // Internal IO helpers
  int readByte() const;
  size_t readBytes(void *buf, size_t count) const;
  bool seekSet(uint32_t pos) const;
  bool seekCur(int32_t offset) const; // Only needed for skip?

  uint16_t readLE16();
  uint32_t readLE32();

  // Source (one is valid)
  FsFile *file = nullptr;
  const uint8_t *memoryBuffer = nullptr;
  size_t memorySize = 0;
  mutable size_t bufferPos = 0;

  bool dithering = false;
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t bfOffBits = 0;
  uint16_t bpp = 0;
  int rowBytes = 0;
  uint8_t paletteLum[256] = {};

  // Floyd-Steinberg dithering state (mutable for const methods)
  mutable int16_t *errorCurRow = nullptr;
  mutable int16_t *errorNextRow = nullptr;
  mutable int prevRowY = -1; // Track row progression for error propagation

  mutable AtkinsonDitherer *atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer *fsDitherer = nullptr;
};
