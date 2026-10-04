#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>

DecodeTarget captureDecodeTarget(const GfxRenderer& renderer, const bool writeFramebuffer) {
  DecodeTarget target;
  target.writeFramebuffer = writeFramebuffer;
  target.framebuffer = renderer.getWriteTarget();
  target.writeOriginY = renderer.getWriteOriginY();
  target.writeRows = renderer.getWriteRows();
  target.renderMode = static_cast<uint8_t>(renderer.getRenderMode());
  target.grayAbsolute = renderer.grayPlanesAreAbsolute();
  target.displayWidthBytes = renderer.getDisplayWidthBytes();
  target.displayWidth = renderer.getDisplayWidth();
  target.displayHeight = renderer.getDisplayHeight();
  target.screenWidth = renderer.getScreenWidth();
  target.screenHeight = renderer.getScreenHeight();
  target.orientation = static_cast<uint8_t>(renderer.getOrientation());
  return target;
}

bool ImageToFramebufferDecoder::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  return decodeToFramebuffer(imagePath, captureDecodeTarget(renderer, !config.cacheOnly), config);
}

bool ImageToFramebufferDecoder::validateAndStoreDimensions(const int64_t width, const int64_t height,
                                                           ImageDimensions& out, const char* format) {
  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid %s dimensions: %lldx%lld", format, static_cast<long long>(width),
            static_cast<long long>(height));
    return false;
  }
  if (width > MAX_SOURCE_DIMENSION || height > MAX_SOURCE_DIMENSION) {
    LOG_ERR("IMG", "%s dimensions exceed supported limit: %lldx%lld (max %lld per dimension)", format,
            static_cast<long long>(width), static_cast<long long>(height),
            static_cast<long long>(MAX_SOURCE_DIMENSION));
    return false;
  }

  const int64_t pixels = width * height;
  if (pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "%s too large (%lldx%lld = %lld pixels), max supported: %lld pixels", format,
            static_cast<long long>(width), static_cast<long long>(height), static_cast<long long>(pixels),
            static_cast<long long>(MAX_SOURCE_PIXELS));
    return false;
  }

  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
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
