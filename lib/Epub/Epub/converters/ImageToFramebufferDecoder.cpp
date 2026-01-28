#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <HardwareSerial.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  if (width > MAX_SOURCE_WIDTH || height > MAX_SOURCE_HEIGHT) {
    Serial.printf("[%lu] [IMG] Image too large (%dx%d %s), max supported: %dx%d\n", millis(), width, height,
                  format.c_str(), MAX_SOURCE_WIDTH, MAX_SOURCE_HEIGHT);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  Serial.printf("[%lu] [IMG] Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.\n",
                millis(), feature.c_str(), imagePath.c_str());
}