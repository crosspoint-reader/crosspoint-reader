#pragma once

#include <cstdint>

#include "MappedInputManager.h"

class WordSelectionInput {
 public:
  enum ButtonEvent : uint8_t {
    INPUT_LEFT = 1,
    INPUT_RIGHT = 2,
    INPUT_UP = 4,
    INPUT_DOWN = 8,
    INPUT_CONFIRM = 16,
    INPUT_BACK = 32,
    INPUT_PREVIOUS = 64,
    INPUT_NEXT = 128
  };
  static constexpr uint32_t BUTTON_REPEAT_MS = 500;

  static uint8_t buttonEdges(const MappedInputManager& input) {
    using B = MappedInputManager::Button;
    return (input.wasPressed(B::ScreenLeft) ? INPUT_LEFT : 0) | (input.wasPressed(B::ScreenRight) ? INPUT_RIGHT : 0) |
           (input.wasPressed(B::ScreenUp) ? INPUT_UP : 0) | (input.wasPressed(B::ScreenDown) ? INPUT_DOWN : 0) |
           (input.wasReleased(B::Confirm) ? INPUT_CONFIRM : 0) | (input.wasReleased(B::Back) ? INPUT_BACK : 0) |
           (input.wasPressed(B::NavPrevious) ? INPUT_PREVIOUS : 0) | (input.wasPressed(B::NavNext) ? INPUT_NEXT : 0);
  }

  uint8_t pollButtons(const MappedInputManager& input, const uint32_t now) {
    uint8_t buttons = buttonEdges(input);
    const uint8_t held = (input.isPressed(MappedInputManager::Button::ScreenLeft) ? INPUT_LEFT : 0) |
                         (input.isPressed(MappedInputManager::Button::ScreenRight) ? INPUT_RIGHT : 0);
    if (held != repeatingButton || (buttons & (INPUT_LEFT | INPUT_RIGHT))) {
      repeatingButton = held;
      lastRepeat = now;
    } else if ((held == INPUT_LEFT || held == INPUT_RIGHT) && now - lastRepeat >= BUTTON_REPEAT_MS) {
      buttons |= held;
      lastRepeat = now;
    }
    return buttons;
  }

 private:
  uint8_t repeatingButton = 0;
  uint32_t lastRepeat = 0;
};
