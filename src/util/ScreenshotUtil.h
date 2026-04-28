#pragma once
#include <GfxRenderer.h>

class ScreenshotUtil {
 public:
  static void takeScreenshot(GfxRenderer& renderer);
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);

  // Called when displayState == FactoryLut. Installs a one-shot hook on the renderer;
  // the caller must then call currentActivity->onScreenshotRequest() to trigger the re-render
  // that fires the hook and captures both grayscale planes.
  static void prepareFactoryLutScreenshot(GfxRenderer& renderer);

 private:
  static void grayscaleHookCallback(const uint8_t* lsbPlane, const uint8_t* msbPlane, int physWidth, int physHeight,
                                    void* ctx);
  static bool saveGrayscaleBmp(const char* filename, const uint8_t* lsbPlane, const uint8_t* msbPlane, int physWidth,
                               int physHeight);
};
