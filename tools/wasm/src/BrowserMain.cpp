#include <HalDisplay.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <emscripten/emscripten.h>
#include <freertos/task.h>
#include <sys/time.h>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <string_view>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SDL.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

extern void setup();
extern void loop();

namespace browser {
void setButton(int index, bool down);
void releaseButtons();
void touch(int phase, int x, int y);
void mouseSwipe(int startX, int startY, int endX, int endY);
const uint32_t* takeFrame();
int rotation();
}  // namespace browser

namespace {
void firmwareTask(void*) {
  setup();
  while (true) {
    gpio.beginFrame();
    loop();
    display.presentIfNeeded();
    SDL_Delay(1);
  }
}
}  // namespace

// Let JS install the virtual SD card before starting setup().
int main() { return 0; }

extern "C" {
EMSCRIPTEN_KEEPALIVE int preview_start() {
  static bool started = false;
  if (started) return 0;
  TaskHandle_t handle = nullptr;
  xTaskCreate(&firmwareTask, "firmware", 8192, nullptr, 1, &handle);
  if (!handle) {
    LOG_ERR("PREVIEW", "Could not start firmware task");
    return 0;
  }
  started = true;
  return 1;
}
EMSCRIPTEN_KEEPALIVE uintptr_t preview_take_frame() { return reinterpret_cast<uintptr_t>(browser::takeFrame()); }
EMSCRIPTEN_KEEPALIVE int preview_step() { return browser::stepTasks(); }
EMSCRIPTEN_KEEPALIVE void preview_stop() { browser::stopTasks(); }
EMSCRIPTEN_KEEPALIVE const char* preview_active_book() {
  return activityManager.isReaderActivity() ? APP_STATE.openEpubPath.c_str() : "";
}
EMSCRIPTEN_KEEPALIVE const char* preview_active_font() { return SETTINGS.sdFontFamilyName; }
EMSCRIPTEN_KEEPALIVE uint32_t preview_path_hash(const char* path) {
  // libc++ uses the same byte hash for string and string_view (the book cache key).
  return std::hash<std::string_view>{}(path);
}
EMSCRIPTEN_KEEPALIVE int preview_width() { return HalDisplay::DISPLAY_WIDTH; }
EMSCRIPTEN_KEEPALIVE int preview_height() { return HalDisplay::DISPLAY_HEIGHT; }
EMSCRIPTEN_KEEPALIVE int preview_rotation() { return browser::rotation(); }
EMSCRIPTEN_KEEPALIVE void preview_button(int index, int down) { browser::setButton(index, down != 0); }
EMSCRIPTEN_KEEPALIVE void preview_release_buttons() { browser::releaseButtons(); }
// Logical screen pixels; phase is down (0), move (1), up (2), or cancel (3).
EMSCRIPTEN_KEEPALIVE void preview_touch(int phase, int x, int y) { browser::touch(phase, x, y); }
EMSCRIPTEN_KEEPALIVE void preview_mouse_swipe(int startX, int startY, int endX, int endY) {
  browser::mouseSwipe(startX, startY, endX, endY);
}

int settimeofday(const struct timeval*, const struct timezone*) {
  errno = EPERM;
  return -1;
}
}
