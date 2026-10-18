#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

namespace {
ScreenshotUtil::Target pendingTarget = ScreenshotUtil::Target::None;
unsigned long pendingDeadline = 0;
constexpr unsigned long kWaitMs = 5000;
}  // namespace

void ScreenshotUtil::request(const Target target) {
  pendingTarget = target;
  pendingDeadline = millis() + kWaitMs;
}

void ScreenshotUtil::service(GfxRenderer& renderer, void (*sendToHost)(const uint8_t* frame, uint32_t size, int width,
                                                                       int height, bool busy)) {
  if (pendingTarget == Target::None) return;
  const Target target = pendingTarget;
  if (static_cast<long>(millis() - pendingDeadline) >= 0) {
    pendingTarget = Target::None;
    LOG_ERR("SCR", "Screenshot skipped: the display stayed busy for %lu ms", kWaitMs);
    if (target == Target::Host) sendToHost(nullptr, 0, 0, 0, /*busy=*/true);
    return;
  }
  if (RenderLock::peek()) return;  // a render pass, or the reader's AA pass, has the buffers
  RenderLock lock;
  // A refresh still uses them. Not isComposingFrame(): render passes hold the lock while they draw, and the
  // reader's pre-render clears the write buffer without ever displaying it, so that marker stays set
  // until it goes stale (2 s), holding every capture back for nothing.
  if (renderer.isRefreshPending()) return;
  pendingTarget = Target::None;
  // The write buffer is not the frame on the panel: after a two-buffer refresh it holds an older
  // frame, and the reader pre-renders its next page into it. So first the panel's frame as the
  // display keeps it, then the activity's correction on top, in the order the light drawer uses
  // (ActivityManager::dispatchLightPanelGesture). The other way round, the copy undid the reader's
  // redraw -- and that copy cannot be trusted while a pre-render is pending: a buffer handed back
  // after a Background-B borrow is seeded from the write buffer, pre-rendered page and all, which
  // is how a capture came out as the next page without its status bar.
  if (renderer.hasSecondaryBuffer()) renderer.syncWriteBufferFromDisplayed();
  activityManager.prepareFramebufferForCapture();
  if (target == Target::Host) {
    sendToHost(renderer.getFrameBuffer(), renderer.getBufferSize(), renderer.getDisplayWidth(),
               renderer.getDisplayHeight(), /*busy=*/false);
  } else {
    takeScreenshot(renderer);
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb) {
    String filename_str = "/screenshots/screenshot-" + String(millis()) + ".bmp";
    if (ScreenshotUtil::saveFramebufferAsBmp(filename_str.c_str(), fb, renderer.getDisplayWidth(),
                                             renderer.getDisplayHeight())) {
      LOG_DBG("SCR", "Screenshot saved to %s", filename_str.c_str());
    } else {
      LOG_ERR("SCR", "Failed to save screenshot");
    }
  } else {
    LOG_ERR("SCR", "Framebuffer not available");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, renderer.getDisplayHeight() - 12, renderer.getDisplayWidth() - 12, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height) {
  if (!framebuffer) {
    return false;
  }

  // Note: the width and height, we rotate the image 90d counter-clockwise to match the default display orientation
  int phyWidth = height;
  int phyHeight = width;

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!dir.empty() && !Storage.exists(dir.c_str())) {
      if (!Storage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  FsFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    file.close();
    Storage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Max row size for 528px height (X3) after rotation = 68 bytes; use fixed buffer to avoid VLA
  constexpr size_t kMaxRowSize = 68;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", rowSizePadded);
    file.close();
    Storage.remove(filename);
    return false;
  }

  // rotate the image 90d counter-clockwise on-the-fly while writing to save memory
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  for (int outY = 0; outY < phyHeight; outY++) {
    for (int outX = 0; outX < phyWidth; outX++) {
      // 90d counter-clockwise: source (srcX, srcY)
      // BMP rows are bottom-to-top, so outY=0 is the bottom of the displayed image
      int srcX = width - 1 - outY;     // phyHeight == width
      int srcY = phyWidth - 1 - outX;  // phyWidth == height
      int fbIndex = srcY * (width / 8) + (srcX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (srcX % 8))) & 0x01;
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
    memset(rowBuffer, 0, rowSizePadded);  // Clear the buffer for the next row
  }

  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
