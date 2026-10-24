#pragma once

#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  using ImageToFramebufferDecoder::decodeToFramebuffer;
  bool decodeToFramebuffer(const std::string& imagePath, const DecodeTarget& target,
                           const RenderConfig& config) override;
  bool decodeToFramebuffer(const std::string& imagePath, const DecodeTarget& target, const RenderConfig& config,
                           bool& cacheWritten) override;
  // Cache-reporting decode for sleep-image conversion, which holds a live
  // renderer; captures it and forwards to the DecodeTarget decode.
  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config,
                           bool& cacheWritten);

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "JPEG"; }
  size_t minFreeHeapToDecode() const override;
};
