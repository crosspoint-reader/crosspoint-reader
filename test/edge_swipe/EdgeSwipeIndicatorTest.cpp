#include <Icon.h>
#include <Logging.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/EdgeSwipe.h"

unsigned long fakeNow = 1000;
unsigned long millis() { return fakeNow; }
struct Rect {
  int x, y, width, height;
  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode { Overlay, Absolute, Direct };
  uint8_t buffer = 0xFF, glass = 0xFF, baseline = 0xFF, pendingFrame = 0xFF;
  bool pending = false, gray = false, inverted = false;
  unsigned refreshes = 0, cleanups = 0;
  void displayBuffer(RefreshMode, bool) {
    if (!gray) assert(baseline == glass);
    glass = baseline = buffer;
    gray = false;
    ++refreshes;
  }
  void displayBufferAsync(RefreshMode) {
    pendingFrame = buffer;
    pending = true;
  }
  void waitRefreshComplete() {
    if (!pending) return;
    assert(buffer == pendingFrame);  // no drawing before the no-shadow refresh finishes
    glass = pendingFrame;
    pending = false;
  }
  void cleanupGrayscaleBuffers(const uint8_t* frame) {
    assert(!pending && !gray && *frame == glass);
    baseline = *frame;
    ++cleanups;
  }
  void displayGrayscaleBase(RefreshMode, bool) {}
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode, bool) { return true; }
  void displayGrayBuffer(bool) { gray = !inverted; }
  bool isInverted() const { return inverted; }
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    *size = 1;
    return &buffer;
  }
  void returnFrameBufferStorage() {}
  uint8_t* getFrameBuffer() { return &buffer; }
};
namespace buildscratch {
void lend(uint8_t*, uint32_t) {}
void reclaim() {}
}  // namespace buildscratch

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum class DisplayContent : uint8_t { Unknown, BW, Grayscale };
  HalDisplay& display;
  mutable DisplayContent displayedContent = DisplayContent::Unknown;
  mutable uint32_t displayGeneration = 0;
  mutable bool asyncRefreshPending = false, absoluteGrayPlanes = false;
  mutable bool promotedRefreshPending_ = false;
  mutable HalDisplay::RefreshMode promotedRefresh_ = HalDisplay::FAST_REFRESH;
  RenderMode renderMode = BW;
  bool fadingFix = false, _stripActive = false;
  uint8_t* frameBuffer;
  uint32_t frameBufferLoans = 0;
  int panelHeight = 480;
  unsigned long start_ms = 0;
  Orientation orientation = Portrait;
  explicit GfxRenderer(HalDisplay& display) : display(display), frameBuffer(&display.buffer) {}
  HalDisplay::RefreshMode applyPromotedRefresh(HalDisplay::RefreshMode mode) const;
  void displayBuffer(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH) const;
  void displayBufferAsync(HalDisplay::RefreshMode mode) const;
  void waitRefreshComplete() const;
  DisplayContent prepareOverlay() const;
  void displayGrayscaleBase(HalDisplay::RefreshMode mode) const;
  bool displayGrayscaleBase(HalDisplay::GrayscaleMode mode, HalDisplay::RefreshMode fallback) const;
  void displayGrayBuffer() const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void writeGrayscalePlaneStrip(bool, const uint8_t*, int, int) const;
  void cleanupGrayscaleWithFrameBuffer() const;
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  uint32_t getDisplayGeneration() const { return displayGeneration; }
  Orientation getOrientation() const { return orientation; }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) const { *t = *r = *b = *l = 0; }
  bool copyRegionToBuffer(int, int, int, int, uint8_t* out, size_t capacity) const {
    assert(capacity > 0);
    *out = *frameBuffer;
    return true;
  }
  bool copyBufferToRegion(int, int, int, int, const uint8_t* in, size_t) const {
    *frameBuffer = *in;
    return true;
  }
};
struct TestFrontlight {
  bool available = false;
  bool present() const { return available; }
} Frontlight;
struct TestTheme {
  const freeink::Icon* lastIcon = nullptr;
  void drawEdgeSwipeTab(const GfxRenderer& renderer, Rect, edge_swipe::Edge, const freeink::Icon& icon) {
    lastIcon = &icon;
    *renderer.frameBuffer = 0x00;
  }
} GUI;

#include "EdgeSwipeIndicator.cpp"

using TickType_t = uint32_t;
using TaskHandle_t = void*;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
enum NotifyAction { eIncrement };
int activityManagerSpinlock = 0;
void taskENTER_CRITICAL(int*) {}
void taskEXIT_CRITICAL(int*) {}
void xTaskNotify(TaskHandle_t, uint32_t, NotifyAction) { assert(false); }
struct RenderLock {
  RenderLock() {}
  ~RenderLock() {}
};
class MappedInputManager {};
class Activity {
 public:
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
};
#include "activities/util/BmpViewerActivity.h"
static_assert(std::is_same_v<decltype(&BmpViewerActivity::render), void (BmpViewerActivity::*)(RenderLock&&)>);
struct HalPowerManager {
  struct Lock {
    Lock() {}
    ~Lock() {}
  };
};
struct {
  uint8_t screenInverted = 0;
} SETTINGS;
struct {
  void setInverted(bool) {}
} display;
struct TestActivity {
  GfxRenderer& renderer;
  bool grayscale;
  unsigned renders = 0;
  void render(RenderLock) {
    ++renders;
    renderer.displayBuffer();
    if (grayscale) renderer.displayGrayBuffer();
  }
};
class ActivityManager {
 public:
  static constexpr uint32_t PAGE_RENDER = 1, INDICATOR_RENDER = 2;
  EdgeSwipeIndicator edgeIndicator;
  EdgeSwipeIndicator::Input edgeIndicatorInput;
  GfxRenderer& renderer;
  TestActivity* currentActivity;
  TaskHandle_t waitingTaskHandle = nullptr;
  ActivityManager(GfxRenderer& renderer, TestActivity& activity) : renderer(renderer), currentActivity(&activity) {}
  [[noreturn]] void renderTaskLoop();
};
struct StopRenderTask {};
ActivityManager* testManager = nullptr;
unsigned taskStep = 0;
bool xTaskNotifyWait(uint32_t entry, uint32_t exit, uint32_t* work, TickType_t timeout) {
  assert(entry == 0 && exit == UINT32_MAX);
  auto& manager = *testManager;
  auto& input = manager.edgeIndicatorInput;
  auto& panel = manager.renderer.display;
  switch (taskStep++) {
    case 0:
      assert(timeout == portMAX_DELAY);
      *work = ActivityManager::INDICATOR_RENDER;
      return true;
    case 1:
      assert(panel.glass == 0x00 && manager.currentActivity->renders == 0);
      fakeNow += 200;
      input.state.stage = edge_swipe::Stage::Idle;  // reverse while the finger is still held
      *work = ActivityManager::INDICATOR_RENDER;
      return true;
    case 2:
      if (!manager.currentActivity->grayscale) {
        assert(timeout == portMAX_DELAY && panel.glass == panel.buffer);
        assert(manager.currentActivity->renders == 0);
        throw StopRenderTask{};
      }
      assert(timeout == 0 && panel.glass == 0x00 && input.state.tracking);
      return false;  // no new notification: the task must restore the page immediately
    case 3:
      assert(manager.currentActivity->renders == 1 && panel.gray && panel.glass == panel.buffer);
      throw StopRenderTask{};
    default:
      assert(false);
      throw StopRenderTask{};
  }
}
#include "GfxRendererDisplay.inc"

void checkTaskRestoration(bool grayscale) {
  fakeNow += 1000;
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.displayBuffer();
  if (grayscale) renderer.displayGrayBuffer();
  TestActivity activity{renderer, grayscale};
  ActivityManager manager(renderer, activity);
  manager.edgeIndicator.begin();
  auto& state = manager.edgeIndicatorInput.state;
  state.contact = 1;
  state.edge = edge_swipe::Edge::Bottom;
  state.position = 240;
  state.tracking = state.claimed = true;
  state.stage = edge_swipe::Stage::Peek;
  testManager = &manager;
  taskStep = 0;
  try {
    manager.renderTaskLoop();
  } catch (const StopRenderTask&) {
    assert(taskStep == (grayscale ? 4u : 3u));
  }
  testManager = nullptr;
}

int main() {
  using namespace edge_swipe;
  using Content = GfxRenderer::DisplayContent;
  HalDisplay display;
  GfxRenderer renderer(display);
  assert(renderer.prepareOverlay() == Content::Unknown);
  renderer.displayBuffer();
  EdgeSwipeIndicator indicator;
  indicator.begin();
  Recognizer rightSwipe;
  rightSwipe.begin(479, 400, 480, 800, Config::DEFAULT_DPI, fakeNow);
  rightSwipe.move(399, 400, fakeNow + 100);
  rightSwipe.release(fakeNow + 150);
  EdgeSwipeIndicator::Input rightInput;
  rightInput.state = rightSwipe.getState();
  const auto ordinarySwipe = display.refreshes;
  indicator.render(renderer, rightInput, fakeNow + 150);
  assert(display.refreshes == ordinarySwipe && indicator.serviceDelay(fakeNow) == EdgeSwipeIndicator::NO_SERVICE);
  EdgeSwipeIndicator::Input input;
  input.state.contact = 1;
  input.state.edge = Edge::Bottom;
  input.state.position = 240;
  input.state.tracking = input.state.claimed = true;
  input.state.stage = Stage::Peek;
  const auto render = [&](Stage stage) {
    input.state.stage = stage;
    indicator.render(renderer, input, fakeNow);
    fakeNow += 200;
    assert(display.buffer == 0xFF);
  };
  render(Stage::Peek);
  assert(display.glass == 0x00);
  assert(GUI.lastIcon == &icon_edge_home_24);
  assert(indicator.serviceDelay(fakeNow) == EdgeSwipeIndicator::NO_SERVICE);
  const auto shown = display.refreshes;
  render(Stage::Armed);
  assert(display.glass == 0x00 && display.refreshes == shown);
  render(Stage::Peek);
  assert(display.refreshes == shown);
  render(Stage::Idle);
  assert(display.glass == 0xFF && indicator.serviceDelay(fakeNow) == EdgeSwipeIndicator::NO_SERVICE);
  render(Stage::Peek);
  render(Stage::Idle);
  const auto exhausted = display.refreshes;
  render(Stage::Armed);
  assert(display.refreshes == exhausted);

  ++input.state.contact;
  render(Stage::Peek);
  input.enabled = false;
  render(Stage::Peek);
  assert(display.glass == 0xFF);
  input.enabled = true;
  ++input.state.contact;
  render(Stage::Peek);
  input.state.tracking = false;
  renderer.displayBuffer();  // a normal repaint absorbs cleanup
  const auto repainted = display.refreshes;
  render(Stage::Idle);
  assert(display.refreshes == repainted && indicator.serviceDelay(fakeNow) == EdgeSwipeIndicator::NO_SERVICE);

  renderer.displayGrayBuffer();
  assert(renderer.prepareOverlay() == Content::Grayscale);
  input.state.tracking = true;
  ++input.state.contact;
  render(Stage::Peek);
  assert(display.refreshes == repainted + 1 && display.glass == 0x00);
  input.state.stage = Stage::Idle;
  assert(indicator.render(renderer, input, fakeNow) == EdgeSwipeIndicator::Cleanup::RedrawPage);
  assert(display.glass == 0x00);  // the activity must redraw, not erase with a B/W frame
  indicator.pageChanged();
  renderer.displayBuffer();
  renderer.displayGrayBuffer();
  assert(display.gray && renderer.prepareOverlay() == Content::Grayscale);
  renderer.displayBuffer();
  assert(renderer.prepareOverlay() == Content::BW);
  renderer.renderMode = GfxRenderer::GRAYSCALE_LSB;
  renderer.copyGrayscaleLsbBuffers();
  assert(renderer.prepareOverlay() == Content::Unknown);
  renderer.renderMode = GfxRenderer::BW;
  renderer.displayBuffer();
  renderer.releaseFrameBufferForBuild();
  assert(renderer.prepareOverlay() == Content::Unknown);
  assert(renderer.restoreFrameBufferAfterBuild());
  assert(renderer.prepareOverlay() == Content::Unknown);
  renderer.displayBuffer();
  assert(renderer.prepareOverlay() == Content::BW);
  renderer.promotedRefreshPending_ = true;
  assert(renderer.prepareOverlay() == Content::Unknown);
  renderer.displayBuffer();

  display.inverted = true;
  renderer.displayGrayBuffer();
  assert(renderer.prepareOverlay() == Content::BW);
  display.inverted = false;
  ++input.state.contact;
  render(Stage::Peek);
  display.buffer = 0xAB;
  renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
  input.state.stage = Stage::Armed;
  indicator.render(renderer, input, fakeNow);
  assert(!display.pending && display.cleanups == 1);
  assert(display.glass == 0x00 && display.buffer == 0xAB);
  fakeNow += 200;
  input.state.tracking = false;
  input.state.stage = Stage::Idle;
  indicator.render(renderer, input, fakeNow);
  assert(display.glass == 0xAB && display.cleanups == 1);

  for (const auto edge : {Edge::Left, Edge::Top}) {
    for (const bool light : {false, true}) {
      fakeNow += 200;
      input.state.edge = edge;
      input.state.tracking = true;
      input.state.committed = false;
      input.state.stage = Stage::Peek;
      ++input.state.contact;
      Frontlight.available = light;
      indicator.render(renderer, input, fakeNow);
      assert(GUI.lastIcon == (edge == Edge::Left ? &icon_edge_back_24
                              : light            ? &icon_edge_light_24
                                                 : &icon_edge_menu_24));
      renderer.displayBuffer();
      indicator.pageChanged();
    }
  }

  fakeNow += 200;
  ++input.state.contact;
  indicator.render(renderer, input, fakeNow);
  const auto beforeCleanup = display.refreshes;
  input.state.tracking = false;
  input.state.committed = true;
  input.state.stage = Stage::Idle;
  fakeNow += 20;
  indicator.render(renderer, input, fakeNow);
  assert(indicator.serviceDelay(fakeNow) == 130);
  fakeNow += 130;
  indicator.render(renderer, input, fakeNow);
  assert(indicator.serviceDelay(fakeNow) == 200);
  assert(indicator.serviceDelay(fakeNow + 199) == 1);
  fakeNow += 200;
  indicator.render(renderer, input, fakeNow);
  assert(display.refreshes == beforeCleanup + 1 && display.glass == 0xAB);
  assert(indicator.serviceDelay(fakeNow) == EdgeSwipeIndicator::NO_SERVICE);
  checkTaskRestoration(false);
  checkTaskRestoration(true);
  std::puts(
      "Indicator icons, deadlines, grayscale restoration, render task, async baseline and framebuffer-loan checks "
      "passed");
}
