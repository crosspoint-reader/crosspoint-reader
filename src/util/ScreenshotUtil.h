#pragma once
#include <GfxRenderer.h>

#include <cstdint>

class ScreenshotUtil {
 public:
  // Where a screenshot goes: a BMP on the card (POWER+DOWN, the reader menu) or the host over serial
  // (CMD:SCREENSHOT).
  enum class Target : uint8_t { None, Card, Host };

  // Asks for a screenshot of the frame on the panel. It is taken by service(), like any other access to
  // the framebuffer: once no render pass holds the buffers and no refresh still uses them. A request
  // made mid-render is served a few loop passes later instead of capturing a frame in flux. A newer
  // request replaces a pending one.
  static void request(Target target);

  // Serves a pending request; the main loop calls it on every pass. `sendToHost` writes the frame to
  // the serial port (main.cpp owns the serial writer). A request the display keeps busy for 5 s is
  // dropped, and a host request then answers SCREENSHOT_ERROR:busy.
  static void service(GfxRenderer& renderer,
                      void (*sendToHost)(const uint8_t* frame, uint32_t size, int width, int height, bool busy));

  static void takeScreenshot(GfxRenderer& renderer);
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);
};
