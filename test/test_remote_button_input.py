"""Run with python3 test/test_remote_button_input.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/MappedInputManager.cpp").read_text()
settings_header = (ROOT / "src/CrossPointSettings.h").read_text()
power_actions = "enum SHORT_PWRBTN {" + settings_header.split(
    "enum SHORT_PWRBTN {", 1
)[1].split("};", 1)[0] + "};"
# Exercise production frame/dispatch methods with physical input under test control.
methods = []
for signature, following in (
    ("void MappedInputManager::update(", "bool MappedInputManager::isNavDirectionSwapped("),
    ("bool MappedInputManager::injectRemoteClick(", "bool MappedInputManager::wasLongPressed("),
    ("bool MappedInputManager::isPressed(", "MappedInputManager::Labels MappedInputManager::mapLabels("),
):
    methods.append(signature + source.split(signature, 1)[1].split(following, 1)[0])

stub = r"""
#pragma once
#include <cstdint>
struct HalGPIO {
  enum { BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER };
  bool pressed = false, released = false, held = false;
  uint8_t button = 0;
  unsigned long powerHeldMs = 20;
  void update() {}
  bool wasAnyPressed() const { return pressed; }
  bool wasAnyReleased() const { return released; }
  bool wasPressed(uint8_t b) const { return b == button && pressed; }
  bool wasReleased(uint8_t b) const { return b == button && released; }
  bool isPressed(uint8_t b) const { return b == button && held; }
  unsigned long getHeldTime() const { return 900; }
  unsigned long getPowerButtonHeldTime() const { return powerHeldMs; }
  bool wasCapacitivePagePressed() const { return false; }
  bool hasHomeKey() const { return false; }
  bool wasHomeKeyTapped() const { return false; }
  bool wasHomeKeyLongPressed() const { return false; }
  bool wasHomeKeyPressed() const { return false; }
};
"""
harness = r"""
#include <cassert>
#include "MappedInputManager.h"
class GfxRenderer {};
namespace BoardConfig {
  bool x4Pro = false;
  bool isX4Pro() { return x4Pro; }
}
unsigned long clockMs = 1000;
unsigned long millis() { return clockMs; }
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
struct CrossPointSettings {
  enum { VIBRATION_OFF, VIBRATION_TOUCH_PAGE };
  POWER_ACTIONS
  int shortPwrBtn = IGNORE, doublePwrBtn = PWR_FRONTLIGHT;
  int vibration = VIBRATION_OFF, hapticIntensity = 0;
  int homeButtonTapAction = 0, homeButtonDoubleTapAction = 0, homeButtonLongPressAction = 0;
} SETTINGS;
struct HalHaptics {
  static void feedback(bool, bool, int) {}
  static void longPress(bool, int) {}
};
bool MappedInputManager::mapButton(Button b, bool (HalGPIO::*fn)(uint8_t) const) const {
  return (gpio.*fn)(static_cast<uint8_t>(b));
}
bool MappedInputManager::wasBackGesture() const { return false; }
MappedInputManager::SwipeDir MappedInputManager::wasSwipe(int*, int*) const { return SwipeDir::None; }
""" + "\n".join(methods) + r"""
int main() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
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
  assert(input.injectRemoteClick(RemoteButton::Back));
}
"""
harness = harness.replace("POWER_ACTIONS", power_actions)
manager_source = (ROOT / "src/activities/ActivityManager.cpp").read_text()
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
""" + remote_turn + r"""
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
harness = harness.replace("  using Button =", "  checkRouting(input);\n  using Button =")
# Remote dispatch must work on both the button board and X4 Pro with click classification enabled.
harness = harness.replace("int main() {", "void checkRemoteButtons() {")
picker = (ROOT / "src/activities/reader/EpubReaderFootnoteSelectActivity.cpp").read_text()
picker_confirm = "  if ((mappedInput.wasReleased(" + picker.split(
    "  if ((mappedInput.wasReleased(", 1
)[1].split(" {", 1)[0]
harness += r"""
#include <vector>
bool pickerConfirms(const MappedInputManager& mappedInput) {
  std::vector<int> footnoteLinks{1};
  PICKER_CONFIRM_CONDITION { return true; }
  return false;
}
void checkPowerClicks() {
  BoardConfig::x4Pro = true;
  SETTINGS.shortPwrBtn = CrossPointSettings::PWR_CONFIRM;
  SETTINGS.doublePwrBtn = CrossPointSettings::PAGE_TURN;
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  gpio.button = HalGPIO::BTN_POWER;
  auto frame = [&](unsigned long time, bool pressed, bool held, bool released, unsigned long duration) {
    clockMs = time;
    gpio.pressed = pressed; gpio.held = held; gpio.released = released; gpio.powerHeldMs = duration;
    input.update();
  };
  frame(2000, true, true, false, 0);
  frame(2020, false, false, true, 20);
  assert(input.homeButtonAction() == HomeButtonAction::Ignore);
  frame(2521, false, false, false, 20);
  assert(input.homeButtonAction() == HomeButtonAction::Confirm);
  frame(2522, false, false, false, 20);
  assert(input.homeButtonAction() == HomeButtonAction::Ignore);
  frame(3000, true, true, false, 0);
  frame(3020, false, false, true, 20);
  frame(3100, true, true, false, 0);
  frame(3120, false, false, true, 20);
  assert(input.homeButtonAction() == HomeButtonAction::NextPage);
  assert(!input.wasPowerSingleClick());
  assert(!pickerConfirms(input));
  frame(3700, false, false, false, 20);
  assert(input.homeButtonAction() == HomeButtonAction::Ignore);
  // A long release must not become Confirm after the normal 400ms short-click limit.
  frame(4000, true, true, false, 0);
  frame(4900, false, false, true, 900);
  assert(input.homeButtonAction() == HomeButtonAction::Ignore);
  assert(!input.wasPowerSingleClick() && !pickerConfirms(input));
  for (unsigned long duration : {301ul, 350ul, 400ul, 401ul}) {
    frame(5000, true, true, false, 0);
    frame(5000 + duration, false, false, true, duration);
    assert(input.wasPowerSingleClick() == (duration <= 400));
    assert(pickerConfirms(input) == (duration <= 400));
    assert((input.homeButtonAction() == HomeButtonAction::Confirm) == (duration <= 400));
  }
  // The picker accepts a single click even when Short click is Ignore.
  SETTINGS.shortPwrBtn = CrossPointSettings::IGNORE;
  frame(6000, true, true, false, 0);
  frame(6020, false, false, true, 20);
  assert(!pickerConfirms(input));
  frame(6521, false, false, false, 20);
  assert(input.wasPowerSingleClick() && pickerConfirms(input));
  assert(input.homeButtonAction() == HomeButtonAction::Ignore);
  frame(6522, false, false, false, 20);
  assert(!input.wasPowerSingleClick());
  // Configured Sleep is still dispatched separately from the single-click marker.
  SETTINGS.shortPwrBtn = CrossPointSettings::SLEEP;
  frame(7000, true, true, false, 0);
  frame(7020, false, false, true, 20);
  assert(!input.powerSleepClick());
  frame(7521, false, false, false, 20);
  assert(input.powerSleepClick());
}

int main() {
  for (const bool x4Pro : {false, true}) {
    BoardConfig::x4Pro = x4Pro;
    checkRemoteButtons();
  }
  checkPowerClicks();
}
"""
harness = harness.replace("PICKER_CONFIRM_CONDITION", picker_confirm)
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
