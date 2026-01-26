#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter;
class PngToFramebufferConverter;

class ImageDecoderFactory {
 public:
  static void initialize();
  // Returns non-owning pointer - factory owns the decoder lifetime
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  static bool isFormatSupported(const std::string& imagePath);
  static std::vector<std::string> getSupportedFormats();

 private:
  static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
  static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
  static bool initialized;
};
