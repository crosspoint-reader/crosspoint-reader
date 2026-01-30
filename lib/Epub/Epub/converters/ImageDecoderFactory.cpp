#include "ImageDecoderFactory.h"

#include <HardwareSerial.h>

#include <memory>
#include <string>
#include <vector>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;
bool ImageDecoderFactory::initialized = false;

void ImageDecoderFactory::initialize() {
  if (initialized) return;

  jpegDecoder = std::unique_ptr<JpegToFramebufferConverter>(new JpegToFramebufferConverter());
  pngDecoder = std::unique_ptr<PngToFramebufferConverter>(new PngToFramebufferConverter());

  initialized = true;
  Serial.printf("[%lu] [DEC] Image decoder factory initialized\n", millis());
}

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  if (!initialized) {
    initialize();
  }

  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  if (jpegDecoder && jpegDecoder->supportsFormat(ext)) {
    return jpegDecoder.get();
  } else if (pngDecoder && pngDecoder->supportsFormat(ext)) {
    return pngDecoder.get();
  }

  Serial.printf("[%lu] [DEC] No decoder found for image: %s\n", millis(), imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  return getDecoder(imagePath) != nullptr;
}

std::vector<std::string> ImageDecoderFactory::getSupportedFormats() {
  std::vector<std::string> formats;
  formats.push_back(".jpg");
  formats.push_back(".jpeg");
  formats.push_back(".png");
  return formats;
}
