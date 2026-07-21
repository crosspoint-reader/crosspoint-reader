
#include <SDL.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <limits>

#include "Arduino.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "SimulatorControls.h"
#include "SimulatorLifecycle.h"

extern void setup();
extern void loop();
extern HalDisplay display; // defined in main.cpp

namespace {
uint32_t environmentMilliseconds(const char *name) {
  const char *value = std::getenv(name);
  if (!value || !*value) return 0;
  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    return 0;
  }
  return static_cast<uint32_t>(parsed);
}
} // namespace

int main(int argc, char **argv) {
  (void)argc;
  SimulatorLifecycle::initProcessArgs(argv);
  setup();
  const uint32_t startedAt = SDL_GetTicks();
  const uint32_t screenshotAfter =
      environmentMilliseconds("CROSSVI_SIM_SCREENSHOT_AFTER_MS");
  const uint32_t exitAfter =
      environmentMilliseconds("CROSSVI_SIM_EXIT_AFTER_MS");
  bool automaticScreenshotTaken = false;
  while (!display.shouldQuit()) {
    // Clear input edge latches once per frame. update() may be called many
    // times within loop(); edges must survive across those calls and only
    // reset here at the frame boundary.
    gpio.beginFrame();
    loop();
    const uint32_t elapsed = SDL_GetTicks() - startedAt;
    if (!automaticScreenshotTaken && screenshotAfter > 0 &&
        elapsed >= screenshotAfter) {
      SimulatorControls::requestScreenshot();
      automaticScreenshotTaken = true;
    }
    // SDL must be driven from the main thread on macOS.
    // The render task writes pixels and sets pendingPresent; we flush them
    // here.
    display.presentIfNeeded();
    if (exitAfter > 0 && elapsed >= exitAfter) break;
    // Yield to the OS so macOS delivers pending keyboard/window events to SDL.
    // Without this, the tight spin-loop starves the Cocoa event system and key
    // presses are only picked up sporadically. 1 ms also caps the loop at ~1
    // kHz, which matches realistic device behaviour (the real ESP32-C3 is
    // limited by FreeRTOS tick rate and e-ink refresh time).
    SDL_Delay(1);
  }
  SDL_Quit();
  // Use _exit() instead of return/exit() to bypass C++ global destructors.
  // `activityManager` (and other globals in main.cpp) are constructed before
  // the render task thread starts, and the render task runs a [[noreturn]]
  // infinite loop.  If normal exit() runs global destructors while the render
  // thread is mid-render, the destructor races with the thread → SIGABRT/
  // SIGSEGV → "quit unexpectedly" dialog.  SDL is already torn down above, so
  // calling _exit(0) here is safe.
  _exit(0);
}
