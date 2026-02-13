#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb) {
    String filename_str = "/screenshots/screenshot-" + String(millis()) + ".bmp";
    if (FsHelpers::saveFramebufferAsBmp(filename_str.c_str(), fb, HalDisplay::DISPLAY_WIDTH,
                                        HalDisplay::DISPLAY_HEIGHT)) {
      LOG_DBG("SCR", "Screenshot saved to %s", filename_str.c_str());
    } else {
      LOG_ERR("SCR", "Failed to save screenshot");
    }
  } else {
    LOG_ERR("SCR", "Framebuffer not available");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, HalDisplay::DISPLAY_HEIGHT - 12, HalDisplay::DISPLAY_WIDTH - 12, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}
