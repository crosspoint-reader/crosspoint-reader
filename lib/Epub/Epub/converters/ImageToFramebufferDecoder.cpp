#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <HardwareSerial.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  if (width * height > MAX_SOURCE_PIXELS) {
    Serial.printf("[%lu] [IMG] Image too large (%dx%d = %d pixels %s), max supported: %d pixels\n", millis(), width,
                  height, width * height, format.c_str(), MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  Serial.printf("[%lu] [IMG] Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.\n",
                millis(), feature.c_str(), imagePath.c_str());
}