#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool originalThresholds = false,
                                         bool gray8 = false);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  // The same picture as 8-bit grayscale, with the same size and crop
  static bool pngFileToGray8BmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to an 8-bit grayscale BMP with the 1-bit thumbnail's size and crop
  static bool pngFileToGray8BmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
