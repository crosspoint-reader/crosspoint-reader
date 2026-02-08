#pragma once
#include <string>

class FsHelpers {
 public:
  static std::string normalisePath(const std::string& path);
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);
};
