#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter;
class PngToFramebufferConverter;

class ImageDecoderFactory {
 public:
  // Returns non-owning pointer - factory owns the decoder lifetime
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  static bool isFormatSupported(const std::string& imagePath);

  // Free-heap floor needed to decode this image, or 0 when the format is not
  // supported. Callers gate on this so they never queue a decode the decoder
  // would refuse; the number is the decoder's own requirement, not a copy.
  static size_t minFreeHeapToDecode(const std::string& imagePath);

 private:
  static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
  static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
};
