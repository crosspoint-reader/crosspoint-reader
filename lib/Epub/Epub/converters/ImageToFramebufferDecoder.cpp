#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  const int64_t totalPixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (totalPixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large (%dx%d = %lld pixels %s), max supported: %d pixels", width, height,
            static_cast<long long>(totalPixels), format.c_str(), MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
