#include <Icon.h>
#include <Logging.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
  bool prepareBwOverlay() const;
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
#include "GfxRendererDisplay.inc"

int main() {
  using namespace edge_swipe;
  HalDisplay display;
  GfxRenderer renderer(display);
  assert(!renderer.prepareBwOverlay());
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
  assert(!renderer.prepareBwOverlay());
  input.state.tracking = true;
  ++input.state.contact;
  render(Stage::Peek);
  assert(display.refreshes == repainted);
  renderer.displayBuffer();
  assert(renderer.prepareBwOverlay());
  renderer.copyGrayscaleLsbBuffers();
  assert(!renderer.prepareBwOverlay());
  renderer.displayBuffer();
  renderer.releaseFrameBufferForBuild();
  assert(!renderer.prepareBwOverlay());
  assert(renderer.restoreFrameBufferAfterBuild());
  assert(!renderer.prepareBwOverlay());
  renderer.displayBuffer();
  assert(renderer.prepareBwOverlay());
  renderer.promotedRefreshPending_ = true;
  assert(!renderer.prepareBwOverlay());
  renderer.displayBuffer();

  display.inverted = true;
  renderer.displayGrayBuffer();
  assert(renderer.prepareBwOverlay());
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
  std::puts(
      "Indicator icons, deadlines, cleanup, repaint, grayscale, async baseline and framebuffer-loan checks passed");
}
