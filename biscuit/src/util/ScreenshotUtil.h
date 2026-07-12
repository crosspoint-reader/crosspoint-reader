#pragma once
#include <GfxRenderer.h>

class ScreenshotUtil {
 public:
  static void takeScreenshot(GfxRenderer& renderer);
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);
};
