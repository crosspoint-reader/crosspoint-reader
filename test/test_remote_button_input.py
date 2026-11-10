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
methods.append("#if FREEINK_CAP_TOUCH\n" + "bool MappedInputManager::wasPowerConfirmClick() const" + source.split(
    "bool MappedInputManager::wasPowerConfirmClick() const", 1
)[1].split("#endif", 1)[0] + "#endif\n")

stub = r"""
#pragma once
#include <cstdint>
struct HalGPIO {
  enum { BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER };
  bool pressed = false, released = false, held = false;
  bool physicalPowerHeld = false;
  uint8_t button = 0;
  unsigned long powerHeldMs = 20;
  void update() {}
  bool wasAnyPressed() const { return pressed; }
  bool wasAnyReleased() const { return released; }
  bool wasPressed(uint8_t b) const { return b == button && pressed; }
  bool wasReleased(uint8_t b) const { return b == button && released; }
  bool isPressed(uint8_t b) const { return b == button && held; }
  bool isPowerButtonPhysicallyPressed() const { return physicalPowerHeld; }
  bool hasTouch() const { return true; }
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
  enum class InputStyle { DigitalButtons, DigitalConfirmPowerHold };
  struct {
    InputStyle inputStyle = InputStyle::DigitalButtons;
    struct { int up = 2, down = 3; } input;
  } ACTIVE;
  bool x4Pro = false;
  bool isX4Pro() { return x4Pro; }
  bool paperMono = false;
  bool isPaperMono() { return paperMono; }
}
unsigned long clockMs = 1000;
unsigned long millis() { return clockMs; }
uint32_t testNow = 1000;
constexpr int LOW = 0;
uint8_t sideButtons = 0;
int digitalRead(int pin) {
  return (sideButtons & (pin == BoardConfig::ACTIVE.input.up ? 1 : 2)) ? LOW : 1;
}
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
struct CrossPointSettings {
  enum { VIBRATION_OFF, VIBRATION_TOUCH_PAGE };
  POWER_ACTIONS
  int shortPwrBtn = IGNORE, doublePwrBtn = PWR_FRONTLIGHT;
  uint16_t getPowerButtonDuration() const { return shortPwrBtn == SLEEP ? 10 : 400; }
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

  // All power-bound reader shortcuts share duration and startup qualification.
  const auto doubleAction = SETTINGS.doublePwrBtn;
  SETTINGS.doublePwrBtn = SETTINGS.IGNORE;
  gpio.button = HalGPIO::BTN_POWER;
  for (int action : {SETTINGS.PWR_BOOKMARK, SETTINGS.PWR_SYNC, SETTINGS.PWR_READER_MENU}) {
    SETTINGS.shortPwrBtn = action;
    for (bool restartHold : {false, true}) {
      input.beginPowerInput(false, restartHold);
      gpio.held = true; gpio.released = false; gpio.powerHeldMs = 100;
      input.update();
      gpio.held = false; gpio.released = true; gpio.physicalPowerHeld = true;
      input.update(true);
      assert((input.homeButtonAction() != HomeButtonAction::Ignore) == !restartHold);
      gpio.released = false; gpio.physicalPowerHeld = false;
      input.update();
      assert((input.homeButtonAction() != HomeButtonAction::Ignore) == !restartHold);
      input.update();
      assert(input.homeButtonAction() == HomeButtonAction::Ignore);
    }
    input.beginPowerInput(false, false);
    gpio.held = true; gpio.powerHeldMs = 0;
    input.update();
    gpio.held = false; gpio.released = true; gpio.powerHeldMs = 1000;
    input.update(true);
    assert(input.homeButtonAction() == HomeButtonAction::Ignore);
    gpio.released = false;
    input.update();
    assert(input.homeButtonAction() == HomeButtonAction::Ignore);
  }

  // Sticky's consumed key remains physically held before logical Power appears.
  SETTINGS.shortPwrBtn = SETTINGS.IGNORE;
  BoardConfig::ACTIVE.inputStyle = BoardConfig::InputStyle::DigitalConfirmPowerHold;
  gpio.released = false; gpio.held = false; gpio.physicalPowerHeld = true;
  input.beginPowerInput(true, true);
  input.update();
  assert(!input.powerSleepRequested() && input.suppressPowerAfterRestart());
  input.beginPowerInput(input.suppressPowerAfterRestart(), true);
  gpio.held = true; gpio.powerHeldMs = 1000;
  input.update();
  assert(!input.powerSleepRequested());
  gpio.held = false; gpio.physicalPowerHeld = false; gpio.released = true;
  input.update();
  assert(input.wasPowerReleaseConsumed() && !input.wasShortPowerReleased());
  assert(!input.powerSleepRequested());

  // Sticky Sleep emits logical Power immediately; work can delay polling until release.
  SETTINGS.shortPwrBtn = SETTINGS.SLEEP;
  input.beginPowerInput(false, false);
  gpio.released = false; gpio.held = true; gpio.powerHeldMs = 0;
  input.update();
  assert(!input.powerSleepRequested());
  gpio.held = false; gpio.released = true; gpio.powerHeldMs = 600;
  input.update();
  assert(input.powerSleepRequested());
  assert(input.wasPowerReleaseConsumed() && !input.wasShortPowerReleased());
  checkSleepDispatch(input, gpio);
  gpio.released = false;
  input.update();
  assert(!input.powerSleepRequested());

  // A shared key released before its logical Power threshold emits Confirm instead.
  SETTINGS.shortPwrBtn = SETTINGS.IGNORE;
  gpio.button = HalGPIO::BTN_CONFIRM;
  gpio.released = true;
  input.beginPowerInput(true, true);
  input.update();
  assert(input.wasPowerReleaseConsumed());
  assert(!input.wasShortPowerReleased());
  input.beginPowerInput(false, false);
  input.update();
  assert(!input.wasPowerReleaseConsumed());
  assert(input.wasReleased(Button::Confirm) && !input.wasShortPowerReleased());
  BoardConfig::ACTIVE.inputStyle = BoardConfig::InputStyle::DigitalButtons;

#if FREEINK_CAP_TOUCH
  SETTINGS.shortPwrBtn = SETTINGS.PWR_CONFIRM;
  gpio.button = HalGPIO::BTN_POWER;
  for (bool suppress : {false, true}) {
    input.beginPowerInput(suppress, suppress);
    gpio.held = true; gpio.released = false; gpio.powerHeldMs = 0;
    input.update();
    gpio.held = false; gpio.released = true; gpio.powerHeldMs = 100;
    input.update();
    assert(input.wasPressed(Button::Confirm) == !suppress);
    assert(input.wasReleased(Button::Confirm) == !suppress);
  }
#endif
  SETTINGS.doublePwrBtn = doubleAction;
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
  input.beginPowerInput(false, false);
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

  // Wake and restart contacts must never enter the configurable click classifier.
  SETTINGS.shortPwrBtn = CrossPointSettings::PWR_SYNC;
  for (const bool suppress : {false, true}) {
    input.beginPowerInput(suppress, true);
    frame(8000, false, true, false, 0);
    frame(8020, false, false, true, 20);
    assert(input.wasPowerReleaseConsumed());
    assert(!input.wasPowerSingleClick() && !pickerConfirms(input));
    frame(8521, false, false, false, 20);
    assert(input.homeButtonAction() == HomeButtonAction::Ignore);
    assert(!input.powerSleepClick() && !input.wasPowerSingleClick());
  }
  // A fresh press after the suppressed contact is released still dispatches normally.
  frame(9000, true, true, false, 0);
  frame(9020, false, false, true, 20);
  frame(9521, false, false, false, 20);
  assert(input.homeButtonAction() == HomeButtonAction::Sync);

  SETTINGS.shortPwrBtn = CrossPointSettings::SLEEP;
  frame(10000, true, true, false, 0);
  frame(10600, false, false, true, 600);
  assert(input.powerSleepRequested() && input.wasPowerReleaseConsumed());
  assert(!input.powerSleepClick() && !input.wasPowerSingleClick());
  checkSleepDispatch(input, gpio);
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
main_source = (ROOT / "src/main.cpp").read_text()
# Rejected wakes must return to sleep before any frontlight restoration.
wake_validation = main_source.index("  switch (wakeupReason) {")
wake_validation_end = main_source.index("\n  }\n", wake_validation) + len("\n  }\n")
frontlight_init = main_source.index("  Frontlight.begin(")
assert wake_validation_end < frontlight_init < main_source.index("  handleSlideSwitch();", wake_validation_end)
release_guard = main_source.split("  if (mappedInputManager.wasPowerReleaseConsumed()) {", 1)[1].split(
    "  if (mappedInputManager.powerSleepClick()) {", 1)[0]
sleep_dispatch = main_source.split("  const bool x4ProAwaitingClickWindow =", 1)[1].split(
    "#if FREEINK_DEVICE_PAPERMONO", 1)[0]
dispatch_harness = r"""
bool slept = false, forwardedRelease = false;
void enterDeepSleep() { slept = true; }
#define LOG_DBG(...) ((void)0)
void dispatchSleep(MappedInputManager& mappedInputManager, HalGPIO& gpio,
                   bool x4ProDoubleClickEnabled, unsigned long allowSleepAt) {
  slept = forwardedRelease = false;
  if (mappedInputManager.wasPowerReleaseConsumed()) {
""" + release_guard + "  const bool x4ProAwaitingClickWindow =" + sleep_dispatch + r"""
  forwardedRelease = true;
}
void checkSleepDispatch(MappedInputManager& input, HalGPIO& gpio) {
  dispatchSleep(input, gpio, false, 0);
  assert(slept && !forwardedRelease);
  dispatchSleep(input, gpio, true, 0);  // A long release is not a double-click candidate.
  assert(slept && !forwardedRelease);
  dispatchSleep(input, gpio, false, millis() + 1);
  assert(!slept && !forwardedRelease);  // Startup delay still consumes the release.
  input.beginPowerInput(true, true);
  input.update();
  dispatchSleep(input, gpio, false, 0);
  assert(!slept && !forwardedRelease);
  input.beginPowerInput(false, false);
  gpio.powerHeldMs = 150;
  input.update();
  dispatchSleep(input, gpio, true, 0);
  assert(!slept);  // Short click still waits for X4 Pro's double-click window.
}
"""
harness = harness.replace("void checkRemoteButtons() {", dispatch_harness + "void checkRemoteButtons() {")
hal_source = (ROOT / "lib/hal/HalGPIO.cpp").read_text()
hal_methods = []
for signature, following in (
    ("void HalGPIO::update()", "bool HalGPIO::wasUsbStateChanged()"),
    ("unsigned long HalGPIO::getPowerButtonHeldTime() const", "bool HalGPIO::hasTouch() const"),
):
    hal_methods.append((signature + hal_source.split(signature, 1)[1].split(following, 1)[0]).replace(
        "HalGPIO::", "PowerClockHarness::"))
clock_harness = r"""
struct PowerClockHarness {
  static constexpr uint8_t BTN_POWER = HalGPIO::BTN_POWER;
  struct {
    bool pressed = false;
    uint32_t powerMs = 0, buttonMs = 0;
    bool held = true;
    void update() {}
    bool wasPressed(uint8_t) const { return pressed; }
    bool isPressed(uint8_t) const { return held; }
    uint8_t capacitivePageButtonMask() const { return 0; }
    uint32_t getPowerButtonHeldTime() const { return powerMs; }
    uint32_t getHeldTime() const { return buttonMs; }
  } inputMgr;
  bool powerButtonClick = false, capacitivePagePressed = false;
  bool usbStateChanged = false, lastUsbConnected = false;
  uint8_t previousCapacitivePageButtons = 0;
  bool isUsbConnected() const { return false; }
  void update();
  unsigned long getPowerButtonHeldTime() const;
};
""" + "\n".join(hal_methods) + r"""
void checkPowerClocks() {
  PowerClockHarness gpio;
  PowerButtonInput input;
#if FREEINK_DEVICE_PAPERMONO
  BoardConfig::paperMono = true;
  // A PMIC pulse remains a click even with an inherited side-button timestamp.
  for (uint8_t sides : {0, 1, 2, 3}) {
    gpio = {};
    sideButtons = sides;
    testNow = 0;
    gpio.inputMgr = {false, 0, 0, false};
    gpio.update();
    input.begin(false, false);
    testNow = sides == 1 ? 1000 : 100;
    gpio.inputMgr = {true, sides ? testNow : 0u, 0};
    gpio.update();
    input.update(true, false, gpio.getPowerButtonHeldTime(), 400);
    assert(!input.shouldSleep());
    testNow += sides == 0 ? 1000 : 500;
    gpio.inputMgr = {false, testNow, 500, false};
    gpio.update();
    input.update(false, true, gpio.getPowerButtonHeldTime(), 400);
    assert(input.wasShortReleased() && !input.wasReleaseConsumed());
  }

  // An early PMIC pulse must not hide the chord that matures on the next poll.
  for (bool earlyClick : {false, true}) {
    gpio = {};
    input.begin(false, false);
    sideButtons = 3;
    testNow = 0;
    gpio.inputMgr = {false, 0, 0, false};
    gpio.update();
    if (earlyClick) {
      testNow = 100;
      gpio.inputMgr = {true, 100, 0};
      gpio.update();
      input.update(true, false, gpio.getPowerButtonHeldTime(), 400);
      assert(!input.shouldSleep());
    }
    testNow = 650;
    gpio.inputMgr = {!earlyClick, 650, 0};
    gpio.update();
    input.update(true, false, gpio.getPowerButtonHeldTime(), 400);
    assert(input.shouldSleep());
    sideButtons = 0;
    testNow = 1000;
    gpio.inputMgr = {false, 1000, 350, false};
    gpio.update();
    input.update(false, true, gpio.getPowerButtonHeldTime(), 400);
    assert(input.wasReleaseConsumed() && !input.wasShortReleased());
  }
#else
  BoardConfig::paperMono = false;
  gpio.inputMgr = {true, 300, 300};
  gpio.update();
  assert(gpio.getPowerButtonHeldTime() == 300);
#endif
}
"""
harness = harness.replace("int main() {", clock_harness + "int main() {\n  checkPowerClocks();")
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / "HalGPIO.h").write_text(stub)
    cpp = directory / "remote_input.cpp"
    executable = directory / "remote_input"
    cpp.write_text("#include <initializer_list>\n" + harness)
    for touch, paper_mono in ((0, 0), (1, 0), (0, 1), (1, 1)):
        subprocess.run([
            "c++", "-std=c++20", f"-DFREEINK_CAP_TOUCH={touch}",
            f"-DFREEINK_DEVICE_PAPERMONO={paper_mono}", "-I", str(directory),
            "-I", str(ROOT / "src"), str(cpp), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
print("Remote button input regression checks passed")
