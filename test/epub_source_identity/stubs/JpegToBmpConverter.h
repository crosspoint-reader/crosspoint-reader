#pragma once

#include <HalStorage.h>

class JpegToBmpConverter {
 public:
  static bool jpegFileToBmpStream(HalFile&, Print&, bool = true) { return false; }
  static bool jpegFileToBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
};
