#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format,
                                                        int64_t maxPixels) {
  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid image dimensions (%dx%d %s)", width, height, format.c_str());
    return false;
  }

  const int64_t totalPixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (totalPixels > maxPixels) {
    LOG_ERR("IMG", "Image too large (%dx%d = %lld pixels %s), max supported: %lld pixels", width, height,
            static_cast<long long>(totalPixels), format.c_str(), static_cast<long long>(maxPixels));
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
