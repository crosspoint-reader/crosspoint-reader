"""Compile MappedInputManager with a live fake GPIO; run with python3 test/test_input_lock.py."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "src/MappedInputManager.h").read_text()
public = "".join(re.findall(r"public:(.*?)(?=\b(?:private|protected):|\Z)", header, re.S))
methods = set(re.findall(r"^\s+(?:\[\[nodiscard\]\]\s+)?(?:[\w:<>,]+[\s*&]+)+(\w+)\(", public, re.M))

# Each query that reports input to the activities. A new public query must be added here and to checks[].
GATED = {
    "wasPressed", "wasReleased", "wasLongPressed", "isPressed", "wasScreenTapped", "wasScreenTouchPressed",
    "wasScreenTouchDown", "wasScreenLongPress", "isScreenTouchHeld", "wasScreenTouchReleased", "rowTouch",
    "colTouch", "wasSwipe", "wasBackGesture", "wasHomeGesture", "homeButtonAction", "wasMenuGesture",
    "wasReaderMenuSwipeUp", "wasLightPanelGesture", "getHeldTime", "getPressedFrontButton",
}
# Not input: the frame pump, capabilities, labels, the release swallow, and remote clicks (main.cpp refuses those).
EXEMPT = {
    "update", "setPowerConfirmClickFrame", "setInputLocked", "injectRemoteClick", "hasRemoteClick",
    "consumeSuppressedRelease", "hasTouch", "resetHomeButtonInput", "getRenderer", "mapLabels",
    "mapDirectionalLabels", "isNavDirectionSwapped",
}
unclassified = methods - GATED - EXEMPT
assert not unclassified, f"classify these MappedInputManager methods in {__file__}: {sorted(unclassified)}"
assert not (GATED | EXEMPT) - methods, f"stale names: {sorted((GATED | EXEMPT) - methods)}"

stubs = {
    "HalGPIO.h": r"""
#pragma once
#include <cstdint>
unsigned long millis();
// Every input the scenario enables is reported on every query.
struct HalGPIO {
  enum { BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER };
  uint8_t buttons = 0;
  bool touch = false, homeKey = false, homeTap = false, pagePress = false, swipe = false;
  float sx = 0, sy = 0, ex = 0, ey = 0;
  void update() {}
  bool hasTouch() const { return true; }
  bool wasPressed(uint8_t b) const { return buttons & (1u << b); }
  bool wasReleased(uint8_t b) const { return buttons & (1u << b); }
  bool isPressed(uint8_t b) const { return buttons & (1u << b); }
  bool isCapacitivePagePressed(uint8_t) const { return false; }
  bool wasAnyPressed() const { return buttons != 0; }
  bool wasAnyReleased() const { return buttons != 0; }
  unsigned long getHeldTime() const { return 2000; }
  unsigned long getPowerButtonHeldTime() const { return 100; }
  unsigned long lastTouchHeldMs() const { return 50; }
  bool wasCapacitivePagePressed() const { return pagePress; }
  bool hasHomeKey() const { return homeKey; }
  bool wasHomeKeyTapped() const { return homeTap; }
  bool wasHomeKeyLongPressed() const { return false; }
  bool wasHomeKeyPressed() const { return homeTap; }
  bool at(float& x, float& y) const { x = 0.5f; y = 0.5f; return touch; }
  bool wasTouchTap(float& x, float& y) const { return at(x, y); }
  bool wasTouchDown(float& x, float& y) const { return at(x, y); }
  bool isTouchTapCandidate(float& x, float& y, unsigned long& heldMs) const { heldMs = 500; return at(x, y); }
  bool wasTouchLongPress(float& x, float& y) const { return at(x, y); }
  bool isTouchHeldAt(float& x, float& y) const { return at(x, y); }
  bool wasTouchReleased() const { return touch; }
  void suppressTouchContact() {}
  bool wasSwipe(float& a, float& b, float& c, float& d) const { a = sx; b = sy; c = ex; d = ey; return swipe; }
};
""",
    "BoardConfig.h": "#pragma once\nnamespace BoardConfig { inline bool isX4Pro() { return false; } }\n",
    "GfxRenderer.h": r"""
#pragma once
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation getOrientation() const { return Portrait; }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  void tapToLogical(float nx, float ny, int& x, int& y) const { x = int(nx * 480); y = int(ny * 800); }
};
""",
    "HalHaptics.h": r"""
#pragma once
#include <cstdint>
inline int hapticPulses = 0;
struct HalHaptics {
  static void feedback(bool enabled, bool pressed, uint8_t) { hapticPulses += enabled && pressed; }
  static void longPress(bool enabled, uint8_t) { hapticPulses += enabled; }
};
""",
    "CrossPointSettings.h": r"""
#pragma once
#include <cstdint>
struct CrossPointSettings {
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PWR_CONFIRM = 5, PWR_BOOKMARK = 7, PWR_SYNC = 8, PWR_READER_MENU = 9 };
  enum VIBRATION { VIBRATION_TOUCH = 0, VIBRATION_TOUCH_PAGE = 1, VIBRATION_OFF = 2 };
  enum { PREV_NEXT, NEXT_PREV, SIDE_BUTTONS_DISABLED, NEXT_NEXT, PREV_PREV };
  uint8_t shortPwrBtn = IGNORE, vibration = VIBRATION_OFF, hapticIntensity = 0, doubleClickPwrLight = 0;
  uint8_t homeButtonTapAction = 1, homeButtonDoubleTapAction = 1, homeButtonLongPressAction = 1;
  uint8_t frontButtonBack = 0, frontButtonConfirm = 1, frontButtonLeft = 2, frontButtonRight = 3;
  uint8_t frontButtonFollowOrientation = 0, sideButtonFollowOrientation = 0, sideButtonLayout = PREV_NEXT;
  uint16_t getPowerButtonDuration() const { return 400; }
};
inline CrossPointSettings SETTINGS;
""",
    "components/UITheme.h": "#pragma once\n",
}

harness = r"""
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "HalHaptics.h"
#include "MappedInputManager.h"
#include "components/HeaderBackTapTarget.h"
unsigned long now = 10000;
unsigned long millis() { return now; }
using Button = MappedInputManager::Button;
using RowTouch = MappedInputManager::RowTouch;
constexpr uint8_t ALL_BUTTONS = 0x7F;
constexpr uint8_t POWER = 1u << HalGPIO::BTN_POWER;

void swipe(HalGPIO& gpio, float sx, float sy, float ex, float ey) {
  gpio.swipe = true;
  gpio.sx = sx; gpio.sy = sy; gpio.ex = ex; gpio.ey = ey;
}

struct Check {
  const char* name;
  void (*arrange)(HalGPIO&);
  bool (*reports)(const MappedInputManager&);
  bool remoteBack = false;  // a remote Back click lands in this pass; main.cpp refuses it once locked
  bool pulsesOnly = false;  // haptics fire in update(), before a pass can decide to lock
};

bool anyButton(const MappedInputManager& in, bool (MappedInputManager::*query)(Button) const) {
  for (uint8_t b = 0; b <= static_cast<uint8_t>(Button::ScreenDown); ++b) {
    if ((in.*query)(static_cast<Button>(b))) return true;
  }
  return false;
}

const Check checks[] = {
  {"wasPressed", [](HalGPIO& g) { g.buttons = ALL_BUTTONS; }, [](const MappedInputManager& in) { return anyButton(in, &MappedInputManager::wasPressed); }},
  {"wasReleased", [](HalGPIO& g) { g.buttons = ALL_BUTTONS; }, [](const MappedInputManager& in) { return anyButton(in, &MappedInputManager::wasReleased); }},
  {"isPressed", [](HalGPIO& g) { g.buttons = ALL_BUTTONS; }, [](const MappedInputManager& in) { return anyButton(in, &MappedInputManager::isPressed); }},
  {"wasLongPressed", [](HalGPIO& g) { g.buttons = ALL_BUTTONS; }, [](const MappedInputManager& in) { return in.wasLongPressed(Button::Confirm, 1000); }},
  {"getHeldTime", [](HalGPIO& g) { g.buttons = ALL_BUTTONS; }, [](const MappedInputManager& in) { return in.getHeldTime() != 0; }},
  {"getPressedFrontButton", [](HalGPIO& g) { g.buttons = ALL_BUTTONS; }, [](const MappedInputManager& in) { return in.getPressedFrontButton() != -1; }},
  {"wasScreenTapped", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int x, y; return in.wasScreenTapped(x, y); }},
  {"wasScreenTouchPressed", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int x, y; return in.wasScreenTouchPressed(x, y); }},
  {"wasScreenTouchDown", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int x, y; return in.wasScreenTouchDown(x, y); }},
  {"wasScreenLongPress", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int x, y; return in.wasScreenLongPress(x, y); }},
  {"isScreenTouchHeld", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int x, y; return in.isScreenTouchHeld(x, y); }},
  {"wasScreenTouchReleased", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { return in.wasScreenTouchReleased(); }},
  {"rowTouch", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int row; return in.rowTouch(row, 0, 100, 8) != RowTouch::None; }},
  {"colTouch", [](HalGPIO& g) { g.touch = true; }, [](const MappedInputManager& in) { int col; return in.colTouch(col, 0, 120, 4, 0, 800) != RowTouch::None; }},
  {"wasSwipe", [](HalGPIO& g) { swipe(g, 0.5f, 0.5f, 0.9f, 0.5f); }, [](const MappedInputManager& in) { return in.wasSwipe() != MappedInputManager::SwipeDir::None; }},
  {"wasBackGesture", [](HalGPIO& g) { swipe(g, 0.05f, 0.5f, 0.6f, 0.5f); }, [](const MappedInputManager& in) { return in.wasBackGesture(); }},
  {"wasBackGesture (header tap)", [](HalGPIO& g) { g.touch = true; HeaderBackTapTarget::set(200, 360, 80, 80); }, [](const MappedInputManager& in) { return in.wasBackGesture(); }},
  {"wasMenuGesture", [](HalGPIO& g) { swipe(g, 0.5f, 0.02f, 0.5f, 0.5f); }, [](const MappedInputManager& in) { return in.wasMenuGesture(); }},
  {"wasLightPanelGesture", [](HalGPIO& g) { swipe(g, 0.5f, 0.02f, 0.5f, 0.5f); }, [](const MappedInputManager& in) { return in.wasLightPanelGesture(); }},
  {"wasHomeGesture (swipe)", [](HalGPIO& g) { swipe(g, 0.5f, 0.98f, 0.5f, 0.5f); }, [](const MappedInputManager& in) { return in.wasHomeGesture(); }},
  {"wasReaderMenuSwipeUp", [](HalGPIO& g) { g.homeKey = true; swipe(g, 0.5f, 0.98f, 0.5f, 0.5f); }, [](const MappedInputManager& in) { return in.wasReaderMenuSwipeUp(); }},
  {"wasHomeGesture (Home key)", [](HalGPIO& g) { g.homeKey = g.homeTap = true; SETTINGS.homeButtonTapAction = 0; }, [](const MappedInputManager& in) { return in.wasHomeGesture(); }},
  {"homeButtonAction (Home key)", [](HalGPIO& g) { g.homeKey = g.homeTap = true; SETTINGS.homeButtonTapAction = 0; }, [](const MappedInputManager& in) { return in.homeButtonAction() != HomeButtonAction::Ignore; }},
  {"homeButtonAction (Power click)", [](HalGPIO& g) { g.buttons = POWER; SETTINGS.shortPwrBtn = CrossPointSettings::PWR_BOOKMARK; }, [](const MappedInputManager& in) { return in.homeButtonAction() != HomeButtonAction::Ignore; }},
  {"Power as Confirm", [](HalGPIO& g) { g.buttons = POWER; SETTINGS.shortPwrBtn = CrossPointSettings::PWR_CONFIRM; }, [](const MappedInputManager& in) { return in.wasPressed(Button::Confirm) || in.wasReleased(Button::Confirm); }},
  {"remote click", [](HalGPIO&) {}, [](const MappedInputManager& in) { return in.wasPressed(Button::Back); }, true},
  {"haptics", [](HalGPIO& g) { g.pagePress = g.homeKey = g.homeTap = true; SETTINGS.vibration = CrossPointSettings::VIBRATION_TOUCH_PAGE; SETTINGS.homeButtonTapAction = 0; }, [](const MappedInputManager&) { return hapticPulses != 0; }, false, true},
};

// One main-loop pass. lockFirst: the lock was on before the pass; otherwise it engages after update(),
// as when this very pass locks.
bool reports(const Check& check, bool locked, bool lockFirst) {
  SETTINGS = CrossPointSettings{};
  hapticPulses = 0;
  HeaderBackTapTarget::clear();
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  check.arrange(gpio);
  if (lockFirst) input.setInputLocked(locked);
  input.update();
  if (check.remoteBack) input.injectRemoteClick(RemoteButton::Back);
  if (!lockFirst) input.setInputLocked(locked);
  return check.reports(input);
}

int main() {
  int failures = 0;
  for (const auto& check : checks) {
    if (!reports(check, false, true)) {
      std::printf("scenario reports no input while unlocked: %s\n", check.name);
      ++failures;
    }
    for (const bool lockFirst : {true, false}) {
      if (lockFirst ? check.remoteBack : check.pulsesOnly) continue;
      if (reports(check, true, lockFirst)) {
        std::printf("input reported while locked (%s): %s\n", lockFirst ? "locked pass" : "locking pass", check.name);
        ++failures;
      }
    }
  }

  // A Home-key tap waiting for its double-tap window does not fire once the lock is off again.
  SETTINGS = CrossPointSettings{};
  SETTINGS.homeButtonTapAction = 0;
  SETTINGS.homeButtonDoubleTapAction = 3;
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  gpio.homeKey = gpio.homeTap = true;
  input.update();
  input.setInputLocked(true);
  gpio.homeTap = false;
  input.setInputLocked(false);
  now += 1000;
  input.update();
  if (input.homeButtonAction() != HomeButtonAction::Ignore) {
    std::printf("a Home-key tap from before the lock fired after it\n");
    ++failures;
  }

  // A long press just before the lock: its release happens while locked, so the next release is a new one.
  gpio = HalGPIO{};
  gpio.buttons = 1u << HalGPIO::BTN_CONFIRM;
  input.update();
  assert(input.wasLongPressed(Button::Confirm, 1000));
  input.setInputLocked(true);
  input.update();
  input.setInputLocked(false);
  input.update();
  if (input.consumeSuppressedRelease()) {
    std::printf("a release after the unlock was swallowed for a long press from before the lock\n");
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
"""

scenarios = set(re.findall(r'^\s*\{"(\w+)', harness, re.M))
assert GATED <= scenarios, f"add a scenario to checks[] for: {sorted(GATED - scenarios)}"

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    for name, text in stubs.items():
        (tmp / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp / name).write_text(text)
    # A copy, so its quoted includes find the stubs before the real src/ headers.
    (tmp / "MappedInputManager.cpp").write_text((ROOT / "src/MappedInputManager.cpp").read_text())
    (tmp / "input_lock.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++20", "-DFREEINK_CAP_TOUCH=1", "-I" + directory, "-I" + str(ROOT / "src"),
                    "-I" + str(ROOT / "freeink-sdk/libs/ui/FreeInkUI/include"), str(tmp / "MappedInputManager.cpp"),
                    str(tmp / "input_lock.cpp"), "-o", str(tmp / "input_lock")], check=True)
    subprocess.run([str(tmp / "input_lock")], check=True)
print(f"Passed: {len(GATED)} input queries report nothing while locked")
