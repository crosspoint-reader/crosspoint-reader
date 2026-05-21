#include "ImageDecoderFactory.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  if (FsHelpers::hasJpgExtension(imagePath)) {
    if (!jpegDecoder) {
      jpegDecoder = makeUniqueNoThrow<JpegToFramebufferConverter>();
      if (!jpegDecoder) {
        LOG_ERR("DEC", "OOM: failed to allocate JPEG decoder");
        return nullptr;
      }
    }
    return jpegDecoder.get();
  }

  if (FsHelpers::hasPngExtension(imagePath)) {
    if (!pngDecoder) {
      pngDecoder = makeUniqueNoThrow<PngToFramebufferConverter>();
      if (!pngDecoder) {
        LOG_ERR("DEC", "OOM: failed to allocate PNG decoder");
        return nullptr;
      }
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  return FsHelpers::hasJpgExtension(imagePath) || FsHelpers::hasPngExtension(imagePath);
}
