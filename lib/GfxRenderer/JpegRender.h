#pragma once

/**
 * @file JpegRender.h
 * @brief Direct 1-bit JPEG rendering for EPUB page images (picojpeg), matching inx behavior.
 */

#include <string>
#include <HalStorage.h>

class GfxRenderer;

class JpegRender {
 public:
  explicit JpegRender(GfxRenderer& renderer) : renderer_(renderer) {}

  bool oneBit(FsFile& jpegFile, int x, int y, int targetWidth, int targetHeight, bool cropToFill = false,
              bool forceExactSize = false) const;
  bool fromPath(const std::string& path, int x, int y, int targetWidth, int targetHeight,
                bool cropToFill = false, bool forceExactSize = false) const;

  static bool getDimensions(FsFile& jpegFile, int* outW, int* outH);
  static bool getDimensions(const std::string& path, int* outW, int* outH);

 private:
  GfxRenderer& renderer_;
};
