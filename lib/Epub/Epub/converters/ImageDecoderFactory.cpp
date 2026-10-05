#include "ImageDecoderFactory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
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

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder.reset(new JpegToFramebufferConverter());
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder.reset(new PngToFramebufferConverter());
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoderForFile(const std::string& imagePath) {
  uint8_t magic[4] = {};
  HalFile file;
  if (Storage.openFileForRead("DEC", imagePath, file)) {
    const int got = file.read(magic, sizeof(magic));
    file.close();
    if (got == static_cast<int>(sizeof(magic))) {
      if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return getDecoder(".jpg");
      if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') return getDecoder(".png");
    }
  }
  return getDecoder(imagePath);
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
