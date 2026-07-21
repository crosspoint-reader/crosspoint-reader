#include "SimulatorControls.h"

#include <cassert>

int main() {
  const auto x4 = SimulatorControls::makeLayout(480, 800);
  assert(x4.windowWidth == 704);
  assert(x4.windowHeight == 832);
  assert(x4.screen.x == 16 && x4.screen.y == 16);
  assert(x4.screen.w == 480 && x4.screen.h == 800);
  assert(x4.sidebar.x == 512 && x4.sidebar.w == 176);

  SimulatorControls::setLayout(x4);
  assert(SimulatorControls::hitTest(x4.screen.x + 20, x4.screen.y + 20) == -1);
  for (int index = 0; index < SimulatorControls::BUTTON_COUNT; ++index) {
    const SDL_Rect &button = x4.buttons[index];
    assert(SimulatorControls::hitTest(button.x + button.w / 2,
                                      button.y + button.h / 2) == index);
    assert(SimulatorControls::hitTest(button.x - 1, button.y) == -1);
  }

  assert(!SimulatorControls::isVisuallyDown(1));
  SimulatorControls::setKeyboardDown(1, true);
  assert(SimulatorControls::isVisuallyDown(1));
  SimulatorControls::setMouseDown(1, true);
  SimulatorControls::setKeyboardDown(1, false);
  assert(SimulatorControls::isVisuallyDown(1));
  SimulatorControls::releaseMouseButtons();
  assert(!SimulatorControls::isVisuallyDown(1));

  SimulatorControls::takeRedrawRequest();
  SimulatorControls::requestScreenshot();
  assert(SimulatorControls::takeRedrawRequest());
  assert(SimulatorControls::takeScreenshotRequest());
  assert(!SimulatorControls::takeScreenshotRequest());

  const auto x3Landscape = SimulatorControls::makeLayout(792, 528);
  assert(x3Landscape.screen.w == 792 && x3Landscape.screen.h == 528);
  assert(x3Landscape.windowWidth == 1016 && x3Landscape.windowHeight == 560);

  const auto x3Portrait = SimulatorControls::makeLayout(528, 792);
  assert(x3Portrait.windowWidth == 752 && x3Portrait.windowHeight == 824);
  assert(x3Portrait.screen.w == 528 && x3Portrait.screen.h == 792);
  SimulatorControls::setLayout(x3Portrait);
  for (int index = 0; index < SimulatorControls::BUTTON_COUNT; ++index) {
    const SDL_Rect &button = x3Portrait.buttons[index];
    assert(SimulatorControls::hitTest(button.x + button.w / 2,
                                      button.y + button.h / 2) == index);
  }
  return 0;
}
