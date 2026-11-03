"""Compile production button mapping; run with python3 test/test_side_button_orientation.py."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/MappedInputManager.cpp").read_text()
mapping = source[source.index("bool MappedInputManager::isNavDirectionSwapped()"):
                 source.index("namespace {", source.index("bool MappedInputManager::mapButton("))]
mapping += source[source.index("MappedInputManager::Labels MappedInputManager::mapLabels("):
                  source.index("int MappedInputManager::getPressedFrontButton()")]
settings = (ROOT / "src/CrossPointSettings.cpp").read_text()
migration_start = settings.index('  if (doc["sideButtonFollowOrientation"].isNull())')
migration = settings[migration_start:settings.index("\n  }", migration_start) + 4]

stub = r"""
#pragma once
#include <cstdint>
struct HalGPIO {
  enum { BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER };
  int pressed = -1;
  bool touch = false;
  bool hasTouch() const { return touch; }
  bool wasPressed(uint8_t b) const { return pressed == b; }
  bool wasReleased(uint8_t b) const { return pressed == b; }
  bool isPressed(uint8_t b) const { return pressed == b; }
};
"""
harness = r"""
#include <cassert>
#include <cstdint>
#include <initializer_list>
#define private public
#include "MappedInputManager.h"
#undef private
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation orientation = Portrait;
  Orientation getOrientation() const { return orientation; }
};
struct CrossPointSettings {
  enum { PREV_NEXT, NEXT_PREV, SIDE_BUTTONS_DISABLED, NEXT_NEXT, PREV_PREV };
  uint8_t frontButtonBack = 0, frontButtonConfirm = 1, frontButtonLeft = 2, frontButtonRight = 3;
  uint8_t frontButtonFollowOrientation = 0, sideButtonFollowOrientation = 0, sideButtonLayout = PREV_NEXT;
} SETTINGS;
struct SavedValue {
  bool missing;
  bool isNull() const { return missing; }
};
struct Document {
  bool missing;
  SavedValue operator[](const char*) const { return {missing}; }
};
bool migrate(bool missing, uint8_t savedSide) {
  Document doc{missing};
  uint8_t sideButtonFollowOrientation = savedSide;
  bool needsResave = false;
""" + migration + r"""
  assert(sideButtonFollowOrientation == (missing ? 0 : savedSide));
  return needsResave;
}
""" + mapping + r"""
int main() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  using Button = MappedInputManager::Button;
  int checks = 0;
  // Columns: portrait, landscape CW, upside-down portrait, landscape CCW.
  constexpr bool expectedSideSwaps[2][4] = {
      {false, false, true, false},
      {false, true, true, true},
  };
  for (bool touch : {false, true}) {
    gpio.touch = touch;
    for (bool front : {false, true}) {
      SETTINGS.frontButtonFollowOrientation = front;
      for (uint8_t side : {0, 1}) {
        SETTINGS.sideButtonFollowOrientation = side;
        assert(migrate(true, side));
        assert(!migrate(false, side));
        for (int orientation = 0; orientation < 4; ++orientation) {
          renderer.orientation = static_cast<GfxRenderer::Orientation>(orientation);
          const bool swapSide = expectedSideSwaps[side][orientation];
          const bool swapNav = (touch || front) && orientation == 2;
          assert(input.isNavDirectionSwapped() == swapNav);
          const char* previous = "previous";
          const char* next = "next";
          const auto labels = input.mapLabels("back", "confirm", previous, next);
          assert(labels.btn3 == (swapNav ? next : previous));
          assert(labels.btn4 == (swapNav ? previous : next));
          for (uint8_t layout = 0; layout < 5; ++layout) {
            SETTINGS.sideButtonLayout = layout;
            for (int physical = 0; physical < 7; ++physical) {
              gpio.pressed = physical;
              const bool up = physical == HalGPIO::BTN_UP;
              const bool down = physical == HalGPIO::BTN_DOWN;
              bool prev = false, next = false;
              switch (layout) {
                case CrossPointSettings::PREV_NEXT:
                  prev = swapSide ? down : up;
                  next = swapSide ? up : down;
                  break;
                case CrossPointSettings::NEXT_PREV:
                  prev = swapSide ? up : down;
                  next = swapSide ? down : up;
                  break;
                case CrossPointSettings::NEXT_NEXT: next = up || down; break;
                case CrossPointSettings::PREV_PREV: prev = up || down; break;
                default: break;
              }
              for (auto query : {&HalGPIO::wasPressed, &HalGPIO::wasReleased, &HalGPIO::isPressed}) {
                assert(input.mapButton(Button::PageBack, query) == prev);
                assert(input.mapButton(Button::PageForward, query) == next);
                assert(input.mapButton(Button::Up, query) == up);
                assert(input.mapButton(Button::Down, query) == down);
                const bool navPrevious = swapNav ? (down || physical == HalGPIO::BTN_RIGHT)
                                                 : (up || physical == HalGPIO::BTN_LEFT);
                const bool navNext = swapNav ? (up || physical == HalGPIO::BTN_LEFT)
                                             : (down || physical == HalGPIO::BTN_RIGHT);
                assert(input.mapButton(Button::NavPrevious, query) == navPrevious);
                assert(input.mapButton(Button::NavNext, query) == navNext);
                ++checks;
              }
            }
          }
        }
      }
    }
  }
  // Verify both button actions against the orientation table for both toggle values.
  gpio.touch = true;
  SETTINGS.sideButtonLayout = CrossPointSettings::PREV_NEXT;
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.orientation = static_cast<GfxRenderer::Orientation>(orientation);
    for (uint8_t side : {0, 1}) {
      SETTINGS.sideButtonFollowOrientation = side;
      const bool swapSide = expectedSideSwaps[side][orientation];
      gpio.pressed = swapSide ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
      assert(input.mapButton(Button::PageBack, &HalGPIO::wasPressed));
      gpio.pressed = swapSide ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
      assert(input.mapButton(Button::PageForward, &HalGPIO::wasPressed));
    }
  }
  assert(checks == 3360);
}
"""
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / "HalGPIO.h").write_text(stub)
    (tmp / "orientation.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + directory,
                    "-I" + str(ROOT / "src"), str(tmp / "orientation.cpp"), "-o", str(tmp / "orientation")], check=True)
    subprocess.run([str(tmp / "orientation")], check=True)
print("Side-button orientation: 3,360 mapping combinations and legacy preference migration passed")
