#pragma once

#include <HalStorage.h>

class Print;

class GifToBmpConverter {
  static bool gifFileToBmpStreamInternal(HalFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true);

 public:
  static bool gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop = true);
  static bool gifFileToBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool gifFileTo1BitBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};