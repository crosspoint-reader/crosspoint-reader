#include "HalGPIO.h"
#include "SimulatorControls.h"

#include <SDL.h>

#include <atomic>
#include <cassert>

std::atomic<bool> quitRequested{false};

namespace {

void pushKey(Uint32 type, SDL_Scancode scancode) {
  SDL_Event event{};
  event.type = type;
  event.key.type = type;
  event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
  event.key.repeat = 0;
  event.key.keysym.scancode = scancode;
  assert(SDL_PushEvent(&event) == 1);
}

void pushMouse(Uint32 type, int x, int y) {
  SDL_Event event{};
  event.type = type;
  event.button.type = type;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.state =
      type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
  event.button.x = x;
  event.button.y = y;
  assert(SDL_PushEvent(&event) == 1);
}

} // namespace

int main() {
  SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
  assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) == 0);
  gpio.begin();

  gpio.beginFrame();
  pushKey(SDL_KEYDOWN, SDL_SCANCODE_RETURN);
  gpio.update();
  assert(gpio.wasPressed(HalGPIO::BTN_CONFIRM));
  assert(gpio.isPressed(HalGPIO::BTN_CONFIRM));

  SDL_Delay(30);
  assert(gpio.getHeldTime() >= 20);

  gpio.beginFrame();
  pushKey(SDL_KEYUP, SDL_SCANCODE_RETURN);
  gpio.update();
  assert(gpio.wasReleased(HalGPIO::BTN_CONFIRM));
  assert(!gpio.isPressed(HalGPIO::BTN_CONFIRM));
  const unsigned long releasedDuration = gpio.getHeldTime();
  assert(releasedDuration >= 20 && releasedDuration < 1000);

  gpio.beginFrame();
  pushKey(SDL_KEYDOWN, SDL_SCANCODE_P);
  gpio.update();
  SDL_Delay(25);
  gpio.beginFrame();
  pushKey(SDL_KEYUP, SDL_SCANCODE_P);
  gpio.update();
  assert(gpio.wasReleased(HalGPIO::BTN_POWER));
  const unsigned long powerDuration = gpio.getPowerButtonHeldTime();
  assert(powerDuration >= 15 && powerDuration < 1000);

  const auto layout = SimulatorControls::makeLayout(528, 792);
  SimulatorControls::setLayout(layout);
  const SDL_Rect &downButton = layout.buttons[HalGPIO::BTN_DOWN];
  gpio.beginFrame();
  pushMouse(SDL_MOUSEBUTTONDOWN, downButton.x + downButton.w / 2,
            downButton.y + downButton.h / 2);
  gpio.update();
  assert(gpio.wasPressed(HalGPIO::BTN_DOWN));
  assert(gpio.isPressed(HalGPIO::BTN_DOWN));
  SDL_Delay(20);
  gpio.beginFrame();
  pushMouse(SDL_MOUSEBUTTONUP, downButton.x + downButton.w / 2,
            downButton.y + downButton.h / 2);
  gpio.update();
  assert(gpio.wasReleased(HalGPIO::BTN_DOWN));
  assert(gpio.getHeldTime() >= 10);

  SDL_Quit();
  return 0;
}
