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
#ifdef POCKET_LIBRARY
  // For a file on the card: by its first bytes (a JPEG or PNG under the other
  // format's name still decodes), else by its extension as getDecoder.
  static ImageToFramebufferDecoder* getDecoderForFile(const std::string& imagePath);
#endif
  static bool isFormatSupported(const std::string& imagePath);

 private:
  static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
  static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
};
