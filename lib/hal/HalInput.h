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

  void setupGpioWakeup();

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

#if CROSSPOINT_EMULATED == 1
using InputManager = HalInput;
#endif
