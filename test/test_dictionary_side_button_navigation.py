"""Exercise production dictionary page navigation against the real button mapping.

Run: python3 test/test_dictionary_side_button_navigation.py (requires a C++20 compiler).
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
mapped_input_source = (ROOT / "src/MappedInputManager.cpp").read_text()
mapping = mapped_input_source[
    mapped_input_source.index("bool MappedInputManager::isNavDirectionSwapped()"):
    mapped_input_source.index("namespace {", mapped_input_source.index("bool MappedInputManager::mapButton("))
]
viewer = (ROOT / "src/activities/reader/DictionaryDefinitionActivity.cpp").read_text()
loop_start = viewer.index("void DictionaryDefinitionActivity::loop()")
nav_start = viewer.index("  const auto frontButtons =", loop_start)
nav_end = viewer.index("\n}\n", nav_start)
navigation = viewer[nav_start:nav_end]
reader = (ROOT / "src/activities/reader/ReaderUtils.h").read_text()
front_buttons = reader[reader.index("struct FrontPageTurnButtons"):reader.index("struct PageTurnResult")]

gpio_stub = r"""
#pragma once
#include <cstdint>
unsigned long millis();
struct HalGPIO {
  enum { BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER };
  int pressed = -1;
  bool touch = false;
  bool pressEdge = false;
  bool releaseEdge = false;
  bool held = false;
  uint32_t heldMs = 0;
  bool hasTouch() const { return touch; }
  bool wasPressed(uint8_t button) const { return pressEdge && pressed == button; }
  bool wasReleased(uint8_t button) const { return releaseEdge && pressed == button; }
  bool isPressed(uint8_t button) const { return held && pressed == button; }
  bool isPowerButtonPhysicallyPressed() const { return isPressed(BTN_POWER); }
};
"""

harness = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "MappedInputManager.h"
#include "util/ButtonNavigator.h"

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation orientation = Portrait;
  Orientation getOrientation() const { return orientation; }
};

struct CrossPointSettings {
  enum { PREV_NEXT, NEXT_PREV, SIDE_BUTTONS_DISABLED, NEXT_NEXT, PREV_PREV };
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonBack = HalGPIO::BTN_BACK, frontButtonConfirm = HalGPIO::BTN_CONFIRM;
  uint8_t frontButtonLeft = HalGPIO::BTN_LEFT, frontButtonRight = HalGPIO::BTN_RIGHT;
  uint8_t frontButtonFollowOrientation = 0, sideButtonFollowOrientation = 0;
} SETTINGS;

static unsigned long clockMs = 1000;
unsigned long millis() { return clockMs; }

""" + mapping + r"""
namespace ReaderUtils {
""" + front_buttons + r"""
}
bool MappedInputManager::wasPressed(Button button) const { return mapButton(button, &HalGPIO::wasPressed); }
bool MappedInputManager::wasReleased(Button button) const { return mapButton(button, &HalGPIO::wasReleased); }
bool MappedInputManager::isPressed(Button button) const { return mapButton(button, &HalGPIO::isPressed); }
unsigned long MappedInputManager::getHeldTime() const { return gpio.heldMs; }

struct DictionaryProbe {
  struct Selection { bool valid = true; };
  struct Content {
    int currentPage = 5;
    int totalPages = 12;
    Selection selection;
  } content;
  MappedInputManager& mappedInput;
  ButtonNavigator buttonNavigator;
  int updates = 0;

  explicit DictionaryProbe(MappedInputManager& input) : mappedInput(input) {}
  void requestUpdate() { ++updates; }
  void navigate() {
    auto currentPage = std::atomic_ref(content.currentPage);
""" + navigation + r"""
  }
};

int main() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  ButtonNavigator::setMappedInputManager(input);
  constexpr bool sideSwaps[2][4] = {
      {false, false, true, false},
      {false, true, true, true},
  };
  int checks = 0;
  for (bool touch : {false, true}) {
    gpio.touch = touch;
    for (bool frontFollow : {false, true}) {
      SETTINGS.frontButtonFollowOrientation = frontFollow;
      for (uint8_t sideFollow : {0, 1}) {
        SETTINGS.sideButtonFollowOrientation = sideFollow;
        for (int orientation = 0; orientation < 4; ++orientation) {
          renderer.orientation = static_cast<GfxRenderer::Orientation>(orientation);
          const bool swapSide = sideSwaps[sideFollow][orientation];
          const bool swapFront = (touch || frontFollow) &&
              (orientation == GfxRenderer::PortraitInverted ||
               orientation == GfxRenderer::LandscapeCounterClockwise);
          for (uint8_t layout = 0; layout < 5; ++layout) {
            SETTINGS.sideButtonLayout = layout;
            for (int physical = 0; physical < 7; ++physical) {
              gpio.pressed = physical;
              const bool up = physical == HalGPIO::BTN_UP;
              const bool down = physical == HalGPIO::BTN_DOWN;
              bool sidePrev = false;
              bool sideNext = false;
              switch (layout) {
                case CrossPointSettings::PREV_NEXT:
                  sidePrev = swapSide ? down : up;
                  sideNext = swapSide ? up : down;
                  break;
                case CrossPointSettings::NEXT_PREV:
                  sidePrev = swapSide ? up : down;
                  sideNext = swapSide ? down : up;
                  break;
                case CrossPointSettings::NEXT_NEXT: sideNext = up || down; break;
                case CrossPointSettings::PREV_PREV: sidePrev = up || down; break;
                default: break;
              }
              const bool next = sideNext || physical == (swapFront ? HalGPIO::BTN_LEFT : HalGPIO::BTN_RIGHT);
              const bool prev = sidePrev || physical == (swapFront ? HalGPIO::BTN_RIGHT : HalGPIO::BTN_LEFT);
              assert(!(next && prev));
              const int delta = static_cast<int>(next) - static_cast<int>(prev);

              gpio.pressEdge = true;
              gpio.releaseEdge = false;
              gpio.held = true;
              gpio.heldMs = 0;
              clockMs = 1000;
              DictionaryProbe probe(input);
              probe.navigate();
              assert(probe.content.currentPage == 5 + delta);
              assert(probe.updates == (delta != 0));
              assert(probe.content.selection.valid == (delta == 0));

              // A held button repeats after the initial delay, not on release.
              gpio.pressEdge = false;
              gpio.heldMs = 601;
              clockMs = 2000;
              probe.navigate();
              assert(probe.content.currentPage == 5 + 2 * delta);
              clockMs = 2200;
              probe.navigate();
              assert(probe.content.currentPage == 5 + 2 * delta);
              clockMs = 2600;
              probe.navigate();
              assert(probe.content.currentPage == 5 + 3 * delta);
              gpio.held = false;
              gpio.releaseEdge = true;
              probe.navigate();
              assert(probe.content.currentPage == 5 + 3 * delta);
              assert(probe.updates == (delta != 0 ? 3 : 0));

              // Page boundaries never wrap, including when both side keys go one way.
              DictionaryProbe boundary(input);
              const int atEdge = delta > 0 ? 11 : 0;
              boundary.content.currentPage = atEdge;
              gpio.pressEdge = true;
              gpio.releaseEdge = false;
              gpio.held = true;
              gpio.heldMs = 0;
              clockMs = 3000;
              boundary.navigate();
              assert(boundary.content.currentPage == atEdge);
              assert(boundary.updates == 0);
              ++checks;
            }
          }
        }
      }
    }
  }
  assert(checks == 1120);
}
"""

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / "HalGPIO.h").write_text(gpio_stub)
    (tmp / "dictionary_buttons.cpp").write_text(harness)
    subprocess.run(
        ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
         "-I" + directory, "-I" + str(ROOT / "src"),
         str(tmp / "dictionary_buttons.cpp"), str(ROOT / "src/util/ButtonNavigator.cpp"),
         "-o", str(tmp / "dictionary_buttons")],
        check=True,
    )
    subprocess.run([str(tmp / "dictionary_buttons")], check=True)

print("Passed: 1,120 dictionary mapping combinations, press/hold/release, and page boundaries")
