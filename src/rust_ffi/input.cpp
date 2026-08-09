// Buttons, touch and gestures, plus the clock the Rust runtime repeats keys on.

#include <MappedInputManager.h>

#include "internal.h"

namespace {

// Mirrors Button on the Rust side. Logical buttons, so the user's front-button
// remapping and the screen orientation are already applied.
bool resolveButton(const uint8_t button, MappedInputManager::Button& out) {
  using Button = MappedInputManager::Button;
  static constexpr Button kButtons[] = {
      Button::Back,        Button::Confirm,    Button::Left,        Button::Right,       Button::Up,
      Button::Down,        Button::Power,      Button::PageBack,    Button::PageForward, Button::NavNext,
      Button::NavPrevious, Button::ScreenLeft, Button::ScreenRight, Button::ScreenUp,    Button::ScreenDown,
  };
  if (button >= sizeof(kButtons) / sizeof(kButtons[0])) return false;
  out = kButtons[button];
  return true;
}

}  // namespace

extern "C" {

uint8_t cpp_input_was_pressed(const uint8_t button) {
  MappedInputManager::Button mapped;
  if (!g_rustInputPtr || !resolveButton(button, mapped)) return 0;
  return g_rustInputPtr->wasPressed(mapped) ? 1 : 0;
}

uint8_t cpp_input_is_pressed(const uint8_t button) {
  MappedInputManager::Button mapped;
  if (!g_rustInputPtr || !resolveButton(button, mapped)) return 0;
  return g_rustInputPtr->isPressed(mapped) ? 1 : 0;
}

uint8_t cpp_input_was_released(const uint8_t button) {
  MappedInputManager::Button mapped;
  if (!g_rustInputPtr || !resolveButton(button, mapped)) return 0;
  return g_rustInputPtr->wasReleased(mapped) ? 1 : 0;
}

uint8_t cpp_input_has_touch() { return g_rustInputPtr && g_rustInputPtr->hasTouch() ? 1 : 0; }

uint8_t cpp_input_was_tapped(int32_t* x, int32_t* y) {
  if (!g_rustInputPtr || !x || !y) return 0;
  int tapX = 0;
  int tapY = 0;
  if (!g_rustInputPtr->wasScreenTapped(tapX, tapY)) return 0;
  *x = tapX;
  *y = tapY;
  return 1;
}

uint8_t cpp_input_touch_held(int32_t* x, int32_t* y) {
  if (!g_rustInputPtr || !x || !y) return 0;
  int heldX = 0;
  int heldY = 0;
  if (!g_rustInputPtr->isScreenTouchHeld(heldX, heldY)) return 0;
  *x = heldX;
  *y = heldY;
  return 1;
}

uint8_t cpp_input_touch_released() { return g_rustInputPtr && g_rustInputPtr->wasScreenTouchReleased() ? 1 : 0; }

uint8_t cpp_input_swipe() {
  if (!g_rustInputPtr) return 0;
  switch (g_rustInputPtr->wasSwipe()) {
    case MappedInputManager::SwipeDir::Left:
      return 1;
    case MappedInputManager::SwipeDir::Right:
      return 2;
    case MappedInputManager::SwipeDir::Up:
      return 3;
    case MappedInputManager::SwipeDir::Down:
      return 4;
    default:
      return 0;
  }
}

uint8_t cpp_input_was_back_gesture() { return g_rustInputPtr && g_rustInputPtr->wasBackGesture() ? 1 : 0; }

uint8_t cpp_input_was_home_gesture() { return g_rustInputPtr && g_rustInputPtr->wasHomeGesture() ? 1 : 0; }

// The only clock Rust has. It exists for key auto-repeat, which ButtonNavigator
// does with millis() on this side.
uint32_t cpp_millis() { return millis(); }
}
