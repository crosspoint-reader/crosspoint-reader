// SDL bridge adapted from CrossPlay (MIT); see ../LICENSE.crossplay.
#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <emscripten/emscripten.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "SDL.h"

namespace {
constexpr size_t PIXELS = HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT;
// Browser-only static buffers: staging, published frame, and a stable JS snapshot.
// None of these are linked into device firmware or allocated during a refresh.
uint32_t staging[PIXELS];
uint32_t published[PIXELS];
uint32_t snapshot[PIXELS];
std::mutex frameMutex;
bool dirty = false;
int stagingRotation = 90;
int publishedRotation = 90;
int snapshotRotation = 90;

constexpr size_t QUEUE_SIZE = 64;
SDL_Event events[QUEUE_SIZE];
std::atomic<size_t> readIndex{0};
std::atomic<size_t> writeIndex{0};
// An even revision makes the queue and requested states a consistent snapshot.
std::atomic<uint32_t> inputRevision{0};
uint8_t keys[SDL_NUM_SCANCODES]{};
std::atomic<uint32_t> requestedKeys{0};
constexpr int BUTTONS[] = {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                           SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN,   SDL_SCANCODE_H};
uint32_t appliedKeys = 0;
constexpr uint32_t TOUCH_CANCEL = 0x8000;
std::atomic<bool> requestedTouchDown{false};
bool appliedTouchDown = false;
int touchX = 0;
int touchY = 0;

void pushEvents(const SDL_Event* pending, size_t count) {
  size_t tail = writeIndex.load(std::memory_order_relaxed);
  const size_t available = (readIndex.load(std::memory_order_acquire) + QUEUE_SIZE - tail - 1) % QUEUE_SIZE;
  if (count > available) return;
  for (size_t i = 0; i < count; ++i) {
    events[tail] = pending[i];
    tail = (tail + 1) % QUEUE_SIZE;
  }
  // A replay must never expose a canceled release followed by an unfinished
  // new contact: the simulator could classify that intermediate state as a tap.
  writeIndex.store(tail, std::memory_order_release);
}
void pushEvent(const SDL_Event& event) { pushEvents(&event, 1); }
}  // namespace

namespace browser {
void setButton(int index, bool down) {
  if (index < 0 || index >= static_cast<int>(sizeof(BUTTONS) / sizeof(BUTTONS[0]))) return;
  if (BoardConfig::isX4Pro() ? index < 4 : index == 6) return;
  const uint32_t mask = 1u << index;
  if (static_cast<bool>(requestedKeys.load() & mask) == down) return;
  inputRevision.fetch_add(1);
  if (down)
    requestedKeys.fetch_or(mask);
  else
    requestedKeys.fetch_and(~mask);
  SDL_Event event{};
  event.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
  event.key.keysym.scancode = BUTTONS[index];
  pushEvent(event);
  inputRevision.fetch_add(1, std::memory_order_release);
}

void touch(int phase, int x, int y) {
  if (!BoardConfig::hasTouch() || phase < 0 || phase > 3) return;
  inputRevision.fetch_add(1);
  SDL_Event event{};
  if (phase == 1) {
    event.motion.type = SDL_MOUSEMOTION;
    event.motion.x = x;
    event.motion.y = y;
  } else {
    requestedTouchDown.store(phase == 0);
    event.button.type = phase == 0 ? SDL_MOUSEBUTTONDOWN : phase == 2 ? SDL_MOUSEBUTTONUP : TOUCH_CANCEL;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x;
    event.button.y = y;
  }
  pushEvent(event);
  inputRevision.fetch_add(1, std::memory_order_release);
}

void mouseSwipe(int startX, int startY, int endX, int endY) {
  if (!BoardConfig::hasTouch()) return;
  SDL_Event swipe[4]{};
  swipe[0].button = {TOUCH_CANCEL, SDL_BUTTON_LEFT, startX, startY};
  swipe[1].button = {SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, startX, startY};
  swipe[2].motion = {SDL_MOUSEMOTION, endX, endY};
  swipe[3].button = {SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, endX, endY};
  inputRevision.fetch_add(1);
  requestedTouchDown.store(false);
  pushEvents(swipe, 4);
  inputRevision.fetch_add(1, std::memory_order_release);
}

void releaseButtons() {
  for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); ++i) setButton(i, false);
}

const uint32_t* takeFrame() {
  // JS reads a stable snapshot of the most recently published frame.
  const std::unique_lock<std::mutex> lock(frameMutex, std::try_to_lock);
  if (!lock.owns_lock() || !dirty) return nullptr;
  std::memcpy(snapshot, published, sizeof(snapshot));
  snapshotRotation = publishedRotation;
  dirty = false;
  return snapshot;
}
int rotation() { return snapshotRotation; }
}  // namespace browser

extern "C" {
int SDL_Init(uint32_t) { return 0; }
void SDL_Quit() {}
const char* SDL_GetError() { return ""; }
SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, uint32_t) { return reinterpret_cast<SDL_Window*>(1); }
SDL_Renderer* SDL_CreateRenderer(SDL_Window*, int, uint32_t) { return reinterpret_cast<SDL_Renderer*>(2); }
SDL_Texture* SDL_CreateTexture(SDL_Renderer*, uint32_t, int, int, int) { return reinterpret_cast<SDL_Texture*>(3); }
int SDL_SetHint(const char*, const char*) { return 1; }
void SDL_SetWindowSize(SDL_Window*, int, int) {}
int SDL_RenderSetLogicalSize(SDL_Renderer*, int, int) { return 0; }
int SDL_GetRendererOutputSize(SDL_Renderer*, int* width, int* height) {
  if (width) *width = HalDisplay::DISPLAY_WIDTH;
  if (height) *height = HalDisplay::DISPLAY_HEIGHT;
  return 0;
}
int SDL_UpdateTexture(SDL_Texture*, const SDL_Rect*, const void* pixels, int pitch) {
  if (!pixels || pitch != HalDisplay::DISPLAY_WIDTH * static_cast<int>(sizeof(uint32_t))) return -1;
  std::memcpy(staging, pixels, sizeof(staging));
  return 0;
}
int SDL_RenderClear(SDL_Renderer*) { return 0; }
int SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) {
  stagingRotation = 0;
  return 0;
}
int SDL_RenderCopyEx(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*, double angle, const SDL_Point*,
                     SDL_RendererFlip) {
  stagingRotation = (static_cast<int>(angle) % 360 + 360) % 360;
  return 0;
}
void SDL_RenderPresent(SDL_Renderer*) {
  const std::lock_guard<std::mutex> lock(frameMutex);
  std::memcpy(published, staging, sizeof(published));
  publishedRotation = stagingRotation;
  dirty = true;
}
int SDL_RenderReadPixels(SDL_Renderer*, const SDL_Rect*, uint32_t, void*, int) { return -1; }
SDL_Surface* SDL_CreateRGBSurfaceWithFormatFrom(void*, int, int, int, int, uint32_t) { return nullptr; }
int SDL_SaveBMP(SDL_Surface*, const char*) { return -1; }
void SDL_FreeSurface(SDL_Surface*) {}
uint32_t SDL_GetTicks() {
  // Narrow an integer so SDL's 32-bit clock wraps instead of saturating.
  return static_cast<uint32_t>(static_cast<uint64_t>(emscripten_get_now()));
}
void SDL_Delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
int SDL_PollEvent(SDL_Event* event) {
  SDL_Event nextEvent{};
  const size_t head = readIndex.load(std::memory_order_relaxed);
  if (head != writeIndex.load(std::memory_order_acquire)) {
    nextEvent = events[head];
    readIndex.store((head + 1) % QUEUE_SIZE, std::memory_order_release);
  } else {
    // Reconcile dropped releases without racing a producer still publishing.
    const uint32_t revision = inputRevision.load(std::memory_order_acquire);
    if (revision & 1u) return 0;
    const uint32_t desired = requestedKeys.load();
    const bool desiredTouch = requestedTouchDown.load();
    if (inputRevision.load(std::memory_order_acquire) != revision || head != writeIndex.load()) return 0;
    if (desired != appliedKeys) {
      for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); ++i) {
        if (!((desired ^ appliedKeys) & (1u << i))) continue;
        nextEvent.key.type = desired & (1u << i) ? SDL_KEYDOWN : SDL_KEYUP;
        nextEvent.key.keysym.scancode = BUTTONS[i];
        break;
      }
    } else if (appliedTouchDown && !desiredTouch) {
      nextEvent.button.type = TOUCH_CANCEL;
      nextEvent.button.button = SDL_BUTTON_LEFT;
      nextEvent.button.x = touchX;
      nextEvent.button.y = touchY;
    } else {
      return 0;
    }
  }
  if (nextEvent.type == SDL_KEYDOWN || nextEvent.type == SDL_KEYUP) {
    const bool down = nextEvent.key.type == SDL_KEYDOWN;
    const int scancode = nextEvent.key.keysym.scancode;
    keys[scancode] = down;
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); ++i) {
      if (BUTTONS[i] == scancode) appliedKeys = down ? appliedKeys | (1u << i) : appliedKeys & ~(1u << i);
    }
  } else if (nextEvent.type == SDL_MOUSEMOTION) {
    touchX = nextEvent.motion.x;
    touchY = nextEvent.motion.y;
  } else {
    if (nextEvent.type == TOUCH_CANCEL) {
      // Suppress selection/swipes on the firmware task before release.
      gpio.suppressTouchContact();
      nextEvent.type = SDL_MOUSEBUTTONUP;
    }
    appliedTouchDown = nextEvent.type == SDL_MOUSEBUTTONDOWN;
    touchX = nextEvent.button.x;
    touchY = nextEvent.button.y;
  }
  if (event) *event = nextEvent;
  return 1;
}
const uint8_t* SDL_GetKeyboardState(int* count) {
  if (count) *count = SDL_NUM_SCANCODES;
  return keys;
}
}
