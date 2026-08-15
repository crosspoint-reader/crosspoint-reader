#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

#ifdef CROSSPOINT_BG_IMAGE_DECODE
#include <atomic>

namespace {
std::atomic<bool> decodeAbort{false};
}  // namespace

void ImageToFramebufferDecoder::requestAbort(const bool abort) { decodeAbort.store(abort, std::memory_order_release); }

bool ImageToFramebufferDecoder::abortRequested() { return decodeAbort.load(std::memory_order_acquire); }
#endif

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  if (width * height > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large (%dx%d = %d pixels %s), max supported: %d pixels", width, height, width * height,
            format.c_str(), MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
