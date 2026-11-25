#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool originalThresholds = false,
                                          bool gray8 = false);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  // The same picture as 8-bit grayscale, with the same size and crop
  static bool jpegFileToGray8BmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
  // Convert to an 8-bit grayscale BMP with the 1-bit thumbnail's size and crop
  static bool jpegFileToGray8BmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                               int targetMaxHeight);
};
