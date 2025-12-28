#pragma once

#include <FS.h>

class ZipFile;

class JpegToBmpConverter {
  static void writeBmpHeader(Print& bmpOut, int width, int height);
  // [COMMENTED OUT] static uint8_t grayscaleTo2Bit(uint8_t grayscale, int x, int y);
  static unsigned char jpegReadCallback(unsigned char* pBuf, unsigned char buf_size,
                                        unsigned char* pBytes_actually_read, void* pCallback_data);

 public:
  static bool jpegFileToBmpStream(File& jpegFile, Print& bmpOut);
};
