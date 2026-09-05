#include "ImageDecoderFactory.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

namespace {
// Detect the real image format by reading a file's leading magic bytes.
// Returns ".jpg"/".png" for a recognised format, or "" (file missing, empty, or
// not a supported image). Lets the reader render images whose path has a missing
// or incorrect extension. The path must point to a file already on storage.
std::string sniffImageExtFromFile(const std::string& imagePath) {
  HalFile file;
  if (!Storage.openFileForRead("DEC", imagePath, file)) {
    return "";
  }
  uint8_t header[8] = {0};
  int read = file.read(header, sizeof(header));
  if (read <= 0) {
    return "";
  }
  return FsHelpers::detectImageExtFromMagic(header, static_cast<size_t>(read));
}
}  // namespace

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

  // Some EPUBs reference images without (or with an incorrect) file extension, so
  // extension-based selection alone fails. Fall back to detecting the real format
  // from the file's magic bytes before giving up.
  if (!JpegToFramebufferConverter::supportsFormat(ext) && !PngToFramebufferConverter::supportsFormat(ext)) {
    std::string sniffed = sniffImageExtFromFile(imagePath);
    if (!sniffed.empty()) {
      ext = sniffed;
    }
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

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
