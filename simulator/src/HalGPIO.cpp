#include "HalGPIO.h"

#include <SDL.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "SimulatorControls.h"
#include "SimulatorLifecycle.h"

// Defined in HalDisplay.cpp — set here so all SDL event polling lives in one
// place.
extern std::atomic<bool> quitRequested;

// Keyboard mapping:
//   BTN_BACK    (0) → Escape
//   BTN_CONFIRM (1) → Return
//   BTN_LEFT    (2) → Left arrow
//   BTN_RIGHT   (3) → Right arrow
//   BTN_UP      (4) → Up arrow
//   BTN_DOWN    (5) → Down arrow
//   BTN_POWER   (6) → P
//   Simulator sleep shortcut → S

static constexpr int NUM_BUTTONS = 7;
static constexpr SDL_Scancode SIMULATOR_SLEEP_SCANCODE = SDL_SCANCODE_S;

static const SDL_Scancode buttonScancode[NUM_BUTTONS] = {
    SDL_SCANCODE_ESCAPE, // BTN_BACK
    SDL_SCANCODE_RETURN, // BTN_CONFIRM
    SDL_SCANCODE_LEFT,   // BTN_LEFT
    SDL_SCANCODE_RIGHT,  // BTN_RIGHT
    SDL_SCANCODE_UP,     // BTN_UP
    SDL_SCANCODE_DOWN,   // BTN_DOWN
    SDL_SCANCODE_P,      // BTN_POWER
};

static bool pressedThisFrame[NUM_BUTTONS] = {};
static bool releasedThisFrame[NUM_BUTTONS] = {};
static bool keyboardHeld[NUM_BUTTONS] = {};
static bool mouseHeld[NUM_BUTTONS] = {};
static unsigned long buttonPressStart = 0;
static unsigned long buttonPressFinish = 0;
static unsigned long powerButtonPressStart = 0;
static unsigned long powerButtonPressFinish = 0;
static bool simulatorSleepRequested = false;
static int activeMouseButton = -1;

static void clearButtonState() {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
    keyboardHeld[i] = false;
    mouseHeld[i] = false;
    SimulatorControls::setKeyboardDown(i, false);
    SimulatorControls::setMouseDown(i, false);
  }
  buttonPressStart = 0;
  buttonPressFinish = 0;
  powerButtonPressStart = 0;
  powerButtonPressFinish = 0;
  activeMouseButton = -1;
}

static int scancodeToButton(SDL_Scancode sc) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonScancode[i] == sc)
      return i;
  }
  return -1;
}

static bool combinedHeld(int button) {
  return keyboardHeld[button] || mouseHeld[button];
}

static bool anyCombinedHeld() {
  for (int button = 0; button < NUM_BUTTONS; ++button) {
    if (combinedHeld(button)) return true;
  }
  return false;
}

static void setInputSource(int button, bool keyboard, bool down) {
  if (button < 0 || button >= NUM_BUTTONS) return;
  const bool anyWasHeld = anyCombinedHeld();
  const bool wasHeld = combinedHeld(button);
  const bool powerWasHeld = combinedHeld(HalGPIO::BTN_POWER);
  if (keyboard) {
    keyboardHeld[button] = down;
    SimulatorControls::setKeyboardDown(button, down);
  } else {
    mouseHeld[button] = down;
    SimulatorControls::setMouseDown(button, down);
  }
  const bool nowHeld = combinedHeld(button);
  const bool anyNowHeld = anyCombinedHeld();
  const bool powerNowHeld = combinedHeld(HalGPIO::BTN_POWER);
  const unsigned long now = SDL_GetTicks();
  if (!wasHeld && nowHeld) {
    pressedThisFrame[button] = true;
  } else if (wasHeld && !nowHeld) {
    releasedThisFrame[button] = true;
  }
  if (!anyWasHeld && anyNowHeld) {
    buttonPressStart = now;
  } else if (anyWasHeld && !anyNowHeld) {
    buttonPressFinish = now;
  }
  if (!powerWasHeld && powerNowHeld) {
    powerButtonPressStart = now;
  } else if (powerWasHeld && !powerNowHeld) {
    powerButtonPressFinish = now;
  }
}

static void releaseActiveMouseButton() {
  if (activeMouseButton >= 0) {
    setInputSource(activeMouseButton, false, false);
    activeMouseButton = -1;
  }
}

static void releaseKeyboardButtons() {
  for (int button = 0; button < NUM_BUTTONS; ++button) {
    if (keyboardHeld[button]) setInputSource(button, true, false);
  }
}

void HalGPIO::begin() {
#if defined(SIMULATOR_DEVICE_X3)
  _deviceType = DeviceType::X3;
#else
  _deviceType = DeviceType::X4;
#endif
}

void HalGPIO::beginFrame() {
  // Clear the press/release edge latches once per frame. See update() for why
  // this is deliberately separate from the SDL poll.
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }
}

void HalGPIO::update() {
  // Per-frame press/release edges are intentionally NOT cleared here; that
  // happens once per frame in beginFrame(). The firmware calls update() several
  // times within a single frame (e.g. CrossPointWebServerActivity polls input
  // between handleClient() bursts, on top of the top-of-loop gpio.update() in
  // main.cpp). If edges were cleared on every update(), a key press drained by
  // an earlier update() would be wiped before a later update()'s wasPressed()
  // check could observe it — which made Back/Exit require repeated presses.
  // Latching edges for the whole frame keeps wasPressed() stable across all
  // update() calls in that frame, matching the on-device InputManager.

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      releaseActiveMouseButton();
      releaseKeyboardButtons();
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      if (e.key.keysym.scancode == SIMULATOR_SLEEP_SCANCODE) {
        simulatorSleepRequested = true;
        continue;
      }
      if (e.key.keysym.scancode == SDL_SCANCODE_F12) {
        SimulatorControls::requestScreenshot();
        continue;
      }
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        setInputSource(btn, true, true);
      }
    } else if (e.type == SDL_KEYUP) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        setInputSource(btn, true, false);
      }
    } else if (e.type == SDL_MOUSEMOTION) {
      SimulatorControls::setHoveredButton(
          SimulatorControls::hitTest(e.motion.x, e.motion.y));
    } else if (e.type == SDL_MOUSEBUTTONDOWN &&
               e.button.button == SDL_BUTTON_LEFT && activeMouseButton < 0) {
      const int button = SimulatorControls::hitTest(e.button.x, e.button.y);
      if (button >= 0) {
        activeMouseButton = button;
        setInputSource(button, false, true);
      }
    } else if (e.type == SDL_MOUSEBUTTONUP &&
               e.button.button == SDL_BUTTON_LEFT) {
      releaseActiveMouseButton();
    } else if (e.type == SDL_WINDOWEVENT &&
               (e.window.event == SDL_WINDOWEVENT_LEAVE ||
                e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
      SimulatorControls::setHoveredButton(-1);
      releaseActiveMouseButton();
      releaseKeyboardButtons();
    }
  }
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return combinedHeld(buttonIndex);
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return pressedThisFrame[buttonIndex];
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return releasedThisFrame[buttonIndex];
}

bool HalGPIO::wasAnyPressed() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pressedThisFrame[i])
      return true;
  }
  return false;
}

bool HalGPIO::wasAnyReleased() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (releasedThisFrame[i])
      return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const {
  if (anyCombinedHeld()) {
    return SDL_GetTicks() - buttonPressStart;
  }
  return buttonPressFinish - buttonPressStart;
}

unsigned long HalGPIO::getPowerButtonHeldTime() const {
  if (combinedHeld(BTN_POWER)) {
    return SDL_GetTicks() - powerButtonPressStart;
  }
  return powerButtonPressFinish - powerButtonPressStart;
}

bool HalGPIO::consumeSimulatorSleepRequest() {
  const bool requested = simulatorSleepRequested;
  simulatorSleepRequested = false;
  return requested;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  if (SimulatorLifecycle::consumeWakeReason() ==
      SimulatorLifecycle::WakeReason::PowerButton) {
    return WakeupReason::PowerButton;
  }
  return WakeupReason::Other;
}
bool HalGPIO::isUsbConnected() const { return true; }
bool HalGPIO::wasUsbStateChanged() const { return false; }
void HalGPIO::startDeepSleep() {
  clearButtonState();

  const char *exitOnSleep = std::getenv("CROSSVI_SIM_EXIT_ON_SLEEP");
  if (exitOnSleep && std::strcmp(exitOnSleep, "1") == 0) {
    quitRequested.store(true);
    return;
  }

  while (true) {
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
      if (e.type == SDL_QUIT) {
        quitRequested.store(true);
        return;
      }

      if (e.type == SDL_KEYDOWN && !e.key.repeat &&
          scancodeToButton(e.key.keysym.scancode) >= 0) {
        clearButtonState();
        SimulatorLifecycle::rebootAsPowerWake();
      }
      if (e.type == SDL_MOUSEBUTTONDOWN &&
          e.button.button == SDL_BUTTON_LEFT &&
          SimulatorControls::hitTest(e.button.x, e.button.y) >= 0) {
        clearButtonState();
        SimulatorLifecycle::rebootAsPowerWake();
      }
    }

    SDL_Delay(10);
  }
}
bool HalGPIO::verifyPowerButtonWakeup(uint16_t /*requiredDurationMs*/,
                                      bool /*shortPressAllowed*/) {
  return true;
}

HalGPIO gpio;
