
#include <SDL.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "Arduino.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "SimulatorControls.h"
#include "SimulatorLifecycle.h"

extern void setup();
extern void loop();
extern HalDisplay display; // defined in main.cpp

namespace {
struct ScriptedKeyEvent {
  uint32_t atMilliseconds = 0;
  SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
  bool down = false;
};

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

SDL_Scancode scriptedScancode(const std::string &name) {
  if (name == "BACK") return SDL_SCANCODE_ESCAPE;
  if (name == "CONFIRM") return SDL_SCANCODE_RETURN;
  if (name == "LEFT") return SDL_SCANCODE_LEFT;
  if (name == "RIGHT") return SDL_SCANCODE_RIGHT;
  if (name == "UP") return SDL_SCANCODE_UP;
  if (name == "DOWN") return SDL_SCANCODE_DOWN;
  if (name == "POWER") return SDL_SCANCODE_P;
  if (name == "SLEEP") return SDL_SCANCODE_S;
  if (name == "SCREENSHOT") return SDL_SCANCODE_F12;
  return SDL_SCANCODE_UNKNOWN;
}

std::vector<ScriptedKeyEvent> scriptedKeyEvents() {
  const char *configured = std::getenv("CROSSVI_SIM_INPUT_SCRIPT");
  if (!configured || !*configured) return {};

  std::vector<ScriptedKeyEvent> events;
  const std::string script(configured);
  size_t start = 0;
  while (start < script.size()) {
    const size_t end = script.find(',', start);
    const std::string token = script.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t separator = token.find(':');
    if (separator != std::string::npos) {
      errno = 0;
      char *timeEnd = nullptr;
      const std::string timeToken = token.substr(0, separator);
      const unsigned long at = std::strtoul(timeToken.c_str(), &timeEnd, 10);
      const size_t durationSeparator = token.find(':', separator + 1U);
      const std::string keyToken = token.substr(
          separator + 1U,
          durationSeparator == std::string::npos ? std::string::npos : durationSeparator - separator - 1U);
      unsigned long duration = 30;
      bool durationValid = true;
      if (durationSeparator != std::string::npos) {
        errno = 0;
        char *durationEnd = nullptr;
        const std::string durationToken = token.substr(durationSeparator + 1U);
        duration = std::strtoul(durationToken.c_str(), &durationEnd, 10);
        durationValid = errno == 0 && durationEnd && *durationEnd == '\0' && duration > 0;
      }
      const SDL_Scancode scancode = scriptedScancode(keyToken);
      if (errno == 0 && timeEnd && *timeEnd == '\0' &&
          durationValid && duration <= std::numeric_limits<uint32_t>::max() &&
          at <= std::numeric_limits<uint32_t>::max() - duration &&
          scancode != SDL_SCANCODE_UNKNOWN) {
        events.push_back({static_cast<uint32_t>(at), scancode, true});
        events.push_back({static_cast<uint32_t>(at + duration), scancode, false});
      }
    }
    if (end == std::string::npos) break;
    start = end + 1U;
  }
  std::stable_sort(events.begin(), events.end(), [](const ScriptedKeyEvent &left, const ScriptedKeyEvent &right) {
    return left.atMilliseconds < right.atMilliseconds;
  });
  return events;
}

void pushScriptedKeyEvent(const ScriptedKeyEvent &scripted) {
  SDL_Event event{};
  event.type = scripted.down ? SDL_KEYDOWN : SDL_KEYUP;
  event.key.type = event.type;
  event.key.state = scripted.down ? SDL_PRESSED : SDL_RELEASED;
  event.key.repeat = 0;
  event.key.keysym.scancode = scripted.scancode;
  event.key.keysym.sym = SDL_GetKeyFromScancode(scripted.scancode);
  SDL_PushEvent(&event);
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
  const std::vector<ScriptedKeyEvent> inputScript = scriptedKeyEvents();
  size_t nextInputEvent = 0;
  bool automaticScreenshotTaken = false;
  while (!display.shouldQuit()) {
    const uint32_t elapsed = SDL_GetTicks() - startedAt;
    while (nextInputEvent < inputScript.size() &&
           inputScript[nextInputEvent].atMilliseconds <= elapsed) {
      pushScriptedKeyEvent(inputScript[nextInputEvent]);
      ++nextInputEvent;
    }
    // Clear input edge latches once per frame. update() may be called many
    // times within loop(); edges must survive across those calls and only
    // reset here at the frame boundary.
    gpio.beginFrame();
    loop();
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
