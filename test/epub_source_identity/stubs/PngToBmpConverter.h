#pragma once

#include <HalStorage.h>

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStream(HalFile&, Print&, bool = true) { return false; }
  static bool pngFileToBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
  static bool pngFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
};
