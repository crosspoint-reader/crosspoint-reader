#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  // 64-bit multiply: a malformed header (e.g. 40000x60000) overflows a plain
  // int product before the comparison can reject it.
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large (%dx%d = %lld pixels %s), max supported: %lld pixels", width, height,
            static_cast<long long>(pixels), format.c_str(), static_cast<long long>(MAX_SOURCE_PIXELS));
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::yieldDuringDecode(uint32_t& lastYieldMs) {
  const uint32_t now = millis();
  if (now - lastYieldMs >= 250) {
    lastYieldMs = now;
    vTaskDelay(1);
  }
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
