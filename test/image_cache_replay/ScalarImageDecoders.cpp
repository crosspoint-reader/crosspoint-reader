#include <ImageOpsConfig.h>

#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PngToFramebufferConverter.h"

static_assert(CROSSPOINT_ACCELERATED_IMAGES == 0);

bool decodeScalarPng(const std::string& path, GfxRenderer& renderer, const RenderConfig& config) {
  PngToFramebufferConverter decoder;
  return decoder.decodeToFramebuffer(path, renderer, config);
}

bool decodeScalarJpeg(const std::string& path, GfxRenderer& renderer, const RenderConfig& config) {
  JpegToFramebufferConverter decoder;
  return decoder.decodeToFramebuffer(path, renderer, config);
}
