"""Run with python3 test/test_remote_button_input.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/MappedInputManager.cpp").read_text()
# Exercise production frame/dispatch methods with physical input under test control.
methods = []
for signature, following in (
    ("void MappedInputManager::update(", "bool MappedInputManager::isNavDirectionSwapped("),
    ("bool MappedInputManager::injectRemoteClick(", "bool MappedInputManager::isPressed("),
    ("bool MappedInputManager::isPressed(", "MappedInputManager::Labels MappedInputManager::mapLabels("),
):
    methods.append(signature + source.split(signature, 1)[1].split(following, 1)[0])

stub = r"""
#pragma once
#include <cstdint>
struct HalGPIO {
  enum : uint8_t { BTN_BACK = 0, BTN_UP = 4, BTN_DOWN = 5, BTN_POWER = 6 };
  bool pressed = false, released = false, held = false;
  uint8_t button = 0;
  uint8_t extraHeld = 0;
  unsigned long heldMs = 900;
  void update() {}
  bool wasAnyPressed() const { return pressed; }
  bool wasAnyReleased() const { return released; }
  bool wasPressed(uint8_t b) const { return b == button && pressed; }
  bool wasReleased(uint8_t b) const { return b == button && released; }
  bool isPressed(uint8_t b) const { return (b == button && held) || (extraHeld & (1u << b)); }
  unsigned long getHeldTime() const { return heldMs; }
  bool wasCapacitivePagePressed() const { return false; }
  bool isCapacitivePagePressed(uint8_t) const { return false; }
  bool hasHomeKey() const { return false; }
  bool wasHomeKeyTapped() const { return false; }
  bool wasHomeKeyLongPressed() const { return false; }
  bool wasHomeKeyPressed() const { return false; }
};
"""
harness = r"""
#include <cassert>
#define LOG_ERR(...) ((void)0)
#include "MappedInputManager.h"
class GfxRenderer {};
unsigned long millis() { return 1000; }
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
struct CrossPointSettings {
  enum { VIBRATION_OFF, VIBRATION_TOUCH_PAGE };
  enum { IGNORE = 0, PWR_BOOKMARK = 7, PWR_SYNC = 8, PWR_READER_MENU = 9 };
  int frontButtonLeft = 2, frontButtonRight = 3, frontButtonBack = 0, frontButtonConfirm = 1;
  int shortPwrBtn = IGNORE;
  int vibration = VIBRATION_OFF, hapticIntensity = 0;
  int homeButtonTapAction = 0, homeButtonDoubleTapAction = 0, homeButtonLongPressAction = 0;
} SETTINGS;
struct HalHaptics {
  static void feedback(bool, bool, int) {}
  static void longPress(bool, int) {}
};
bool MappedInputManager::mapButton(Button b, bool (HalGPIO::*fn)(uint8_t) const) const {
  switch (b) {
    case Button::Confirm: return readButton(SETTINGS.frontButtonConfirm, fn);
    case Button::Back: return readButton(SETTINGS.frontButtonBack, fn);
    case Button::Left: return readButton(SETTINGS.frontButtonLeft, fn);
    case Button::Right: return readButton(SETTINGS.frontButtonRight, fn);
    default: return readButton(static_cast<uint8_t>(b), fn);
  }
}
bool MappedInputManager::hasTouch() const { return false; }
bool MappedInputManager::wasBackGesture() const { return false; }
MappedInputManager::SwipeDir MappedInputManager::wasSwipe(int*, int*) const { return SwipeDir::None; }
""" + "\n".join(methods) + r"""
int main() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  checkRouting(input);
  using Button = MappedInputManager::Button;
  struct Mapping { RemoteButton remote; Button button; Button other; };
  for (const auto mapping : {
      Mapping{RemoteButton::Confirm, Button::Confirm, Button::Back},
      Mapping{RemoteButton::Back, Button::Back, Button::Confirm},
      Mapping{RemoteButton::Next, Button::NavNext, Button::NavPrevious},
      Mapping{RemoteButton::Next, Button::Right, Button::Left},
      Mapping{RemoteButton::Previous, Button::NavPrevious, Button::NavNext},
      Mapping{RemoteButton::Previous, Button::Left, Button::Right}}) {
    const auto remote = mapping.remote;
    const auto button = mapping.button;
    const auto other = mapping.other;
    input.update();
    assert(input.injectRemoteClick(remote));
    assert(input.hasRemoteClick());
    assert(input.wasPressed(button) && input.wasReleased(button));
    assert(!input.wasPressed(other) && !input.wasReleased(other));
    assert(!input.isPressed(button) && input.getHeldTime() == 0);
    assert(!input.injectRemoteClick(remote));
    input.update();
    assert(!input.hasRemoteClick());
    assert(!input.wasPressed(button) && !input.wasReleased(button));
    assert(input.getHeldTime() == 900);
  }
  // Physical edges and held Confirm/Back must win over a simultaneous remote click.
  gpio.pressed = true;
  assert(!input.injectRemoteClick(RemoteButton::Confirm));
  gpio.pressed = false;
  gpio.released = true;
  assert(!input.injectRemoteClick(RemoteButton::Back));
  gpio.released = false;
  gpio.held = true;
  for (Button button : {Button::Confirm, Button::Back}) {
    gpio.button = static_cast<uint8_t>(button);
    assert(!input.injectRemoteClick(RemoteButton::Confirm));
    assert(!input.injectRemoteClick(RemoteButton::Back));
  }
  gpio.held = false;
  input.update();
  input.resetButtonBuffer(true);
  gpio.button = static_cast<uint8_t>(Button::Confirm);
  gpio.pressed = gpio.held = true;
  input.update();
  gpio.pressed = gpio.held = false;
  input.update();
  assert(!input.injectRemoteClick(RemoteButton::Next));
  input.resetButtonBuffer(true);
  gpio.button = 2;  // Held directional contact without a new physical edge.
  gpio.held = true;
  input.update();
  assert(input.beginBufferedButtons());
  assert(input.bufferedNavigationOnly());
  assert(input.getHeldTime() == 900);
  assert(input.injectRemoteClick(RemoteButton::Back));
  assert(input.wasPressed(Button::Back) && input.wasReleased(Button::Back));
  assert(input.getHeldTime() == 0);
  assert(!input.bufferedNavigationOnly());
  input.endBufferedButtons();

  // A hold entirely queued during a repaint clears a mapping without activating it.
  for (int confirm : {1, 3}) {
    SETTINGS.frontButtonConfirm = confirm;
    gpio = {};
    input.resetButtonBuffer(true);
    gpio.button = confirm;
    gpio.pressed = gpio.held = true;
    gpio.heldMs = 0;
    input.update();
    gpio.pressed = gpio.held = false;
    gpio.released = true;
    gpio.heldMs = 900;
    input.update();
    assert(input.beginBufferedButtons());
    assert(!input.wasLongPressed(Button::Confirm, 700));
    input.endBufferedButtons();
    assert(input.beginBufferedButtons());
    assert(input.wasLongPressed(Button::Confirm, 700));
    assert(!input.wasReleased(Button::Confirm));
    assert(!input.wasLongPressed(Button::Confirm, 700));
    input.endBufferedButtons();
    // A fresh short click must still activate, without stale suppression.
    gpio.pressed = gpio.held = true;
    gpio.released = false;
    gpio.heldMs = 0;
    input.update();
    assert(input.beginBufferedButtons());
    input.endBufferedButtons();
    gpio.pressed = gpio.held = false;
    gpio.released = true;
    gpio.heldMs = 50;
    input.update();
    assert(input.beginBufferedButtons());
    assert(!input.consumeSuppressedRelease());
    assert(!input.wasLongPressed(Button::Confirm, 700));
    assert(input.wasReleased(Button::Confirm));
    input.endBufferedButtons();
  }
  SETTINGS.frontButtonConfirm = 1;
}
"""
manager_source = (ROOT / "src/activities/ActivityManager.cpp").read_text()
screenshot = "  static bool screenshotButtonsReleased" + (ROOT / "src/main.cpp").read_text().split(
    "  static bool screenshotButtonsReleased", 1
)[1].split("\n  // Consume the second X4 Pro", 1)[0]
remote_turn = "bool ActivityManager::remoteTurn(" + manager_source.split(
    "bool ActivityManager::remoteTurn(", 1
)[1].split("bool ActivityManager::handleForcedRefresh()", 1)[0]
harness = harness.replace("int main() {", r"""
#include <atomic>
#include <memory>
#include "activities/reader/RemoteTurnGate.h"
struct RenderLock { static bool peek() { return false; } };
struct ReaderActivity {
  std::atomic<bool> pageRendered{true};
  bool menu = false;
  int pages = 0, chapters = 0;
  bool inputOverPage() const { return menu; }
  bool pageTurn(bool forward) { pages += forward ? 1 : -1; return true; }
  bool skipPages(int step) { chapters += step; return true; }
};
struct ActivityManager {
  MappedInputManager& mappedInput;
  std::unique_ptr<ReaderActivity> currentActivity = std::make_unique<ReaderActivity>();
  bool context = true, foregroundReader = true;
  bool isRemoteReaderContext() const { return context; }
  bool isForegroundReader() const { return foregroundReader; }
  bool remoteClick(RemoteButton button) { return mappedInput.injectRemoteClick(button); }
  void requestUpdate() {}
  bool remoteTurn(bool forward, bool chapter);
};
struct ScreenshotUtil {
  static inline int count = 0;
  static void takeScreenshot(GfxRenderer&) { ++count; }
};
void handleScreenshot(HalGPIO& gpio, GfxRenderer& renderer, MappedInputManager& mappedInputManager) {
""" + screenshot + r"""
}
""" + remote_turn + r"""
void checkScreenshot() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  using Button = MappedInputManager::Button;
  for (bool powerReleasedFirst : {false, true}) {
    gpio = {};
    input.resetButtonBuffer(true);
    gpio.button = HalGPIO::BTN_DOWN;
    gpio.pressed = gpio.held = true;
    input.update();  // Down was captured before the global shortcut runs.
    gpio.extraHeld = 1u << HalGPIO::BTN_POWER;
    input.update();
    int before = ScreenshotUtil::count;
    handleScreenshot(gpio, renderer, input);
    handleScreenshot(gpio, renderer, input);
    assert(ScreenshotUtil::count == before + 1);
    gpio.pressed = false;
    gpio.button = powerReleasedFirst ? HalGPIO::BTN_POWER : HalGPIO::BTN_DOWN;
    gpio.held = false;
    gpio.released = true;
    gpio.extraHeld = 1u << (powerReleasedFirst ? HalGPIO::BTN_DOWN : HalGPIO::BTN_POWER);
    input.update();
    handleScreenshot(gpio, renderer, input);
    assert(input.beginBufferedButtons());
    assert(!input.wasPressed(Button::Down) && !input.wasReleased(Button::Down));
    input.endBufferedButtons();
    gpio.button = powerReleasedFirst ? HalGPIO::BTN_DOWN : HalGPIO::BTN_POWER;
    gpio.extraHeld = 0;
    input.update();
    handleScreenshot(gpio, renderer, input);
    assert(input.beginBufferedButtons());
    assert(!input.wasPressed(Button::Down) && !input.wasReleased(Button::Down));
    assert(!input.wasReleased(Button::Power));
    input.endBufferedButtons();
    // A new Down contact still navigates after both shortcut contacts end.
    gpio.button = HalGPIO::BTN_DOWN;
    gpio.released = false;
    gpio.pressed = gpio.held = true;
    input.update();
    assert(input.beginBufferedButtons());
    assert(input.wasPressed(Button::Down));
    input.endBufferedButtons();
  }
}
void checkRouting(MappedInputManager& input) {
  ActivityManager manager{input};
  input.update();
  assert(manager.remoteTurn(true, false));
  assert(manager.currentActivity->pages == 1 && !input.hasRemoteClick());
  for (bool reader : {true, false}) {
    manager.foregroundReader = reader;
    manager.currentActivity->menu = true;
    for (bool forward : {true, false}) {
      input.update();
      assert(manager.remoteTurn(forward, false));
      assert(input.wasPressed(forward ? MappedInputManager::Button::NavNext
                                     : MappedInputManager::Button::NavPrevious));
      assert(manager.currentActivity->pages == 1);
      assert(!manager.remoteTurn(forward, false));
    }
    input.update();
    assert(!manager.remoteTurn(true, true));
    assert(manager.currentActivity->chapters == 0 && !input.hasRemoteClick());
  }
  manager.context = false;
  assert(!manager.remoteTurn(true, false));
}
int main() {
""")
harness = harness.replace("int main() {", "int main() {\n  checkScreenshot();")
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / "HalGPIO.h").write_text(stub)
    cpp = directory / "remote_input.cpp"
    executable = directory / "remote_input"
    cpp.write_text("#include <initializer_list>\n" + harness)
    subprocess.run([
        "c++", "-std=c++20", "-DFREEINK_CAP_TOUCH=0", "-I", str(directory),
        "-I", str(ROOT / "src"), str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Remote button input regression checks passed")
