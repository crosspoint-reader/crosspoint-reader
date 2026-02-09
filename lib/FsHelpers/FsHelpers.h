#pragma once
#include <string>

class FsHelpers {
 public:
  static std::string normalisePath(const std::string& path);
  // Note: the image will be rotated 90d counter-clockwise to match the default display orientation
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);
};
