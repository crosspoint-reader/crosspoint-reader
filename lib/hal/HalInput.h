#pragma once

#include <Arduino.h>

#if CROSSPOINT_EMULATED == 0
#include <InputManager.h>
#endif

class HalInput {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

 public:
  HalInput() = default;
  void begin();

  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

 private:
  // emulation state
  uint8_t currentState;
  uint8_t lastState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;
};

// TODO @ngxson : this is a trick to avoid changing too many files at once.
// consider refactoring in a dedicated PR later.
#if CROSSPOINT_EMULATED == 1
using InputManager = HalInput;
#endif

void startDeepSleep(InputManager& inputMgr);
