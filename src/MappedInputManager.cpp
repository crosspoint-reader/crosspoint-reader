#include "MappedInputManager.h"

#include "CrossPointSettings.h"

decltype(InputManager::BTN_BACK) MappedInputManager::mapButton(const Button button) const {
  const auto frontLayout = static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(SETTINGS.frontButtonLayout);
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);

  switch (button) {
    case Button::Back:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          return InputManager::BTN_LEFT;
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          return InputManager::BTN_CONFIRM;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
          /* fall through */
        case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
          /* fall through */
        default:
          return InputManager::BTN_BACK;
      }
    case Button::Confirm:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          return InputManager::BTN_RIGHT;
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          return InputManager::BTN_LEFT;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
          /* fall through */
        case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
          /* fall through */
        default:
          return InputManager::BTN_CONFIRM;
      }
    case Button::Left:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          /* fall through */
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          return InputManager::BTN_BACK;
        case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
          return InputManager::BTN_RIGHT;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
          /* fall through */
        default:
          return InputManager::BTN_LEFT;
      }
    case Button::Right:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          return InputManager::BTN_CONFIRM;
        case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
          return InputManager::BTN_LEFT;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
          /* fall through */
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          /* fall through */
        default:
          return InputManager::BTN_RIGHT;
      }
    case Button::Up:
      return InputManager::BTN_UP;
    case Button::Down:
      return InputManager::BTN_DOWN;
    case Button::Power:
      return InputManager::BTN_POWER;
    case Button::PageBack:
      switch (sideLayout) {
        case CrossPointSettings::NEXT_PREV:
          return InputManager::BTN_DOWN;
        case CrossPointSettings::PREV_NEXT:
          /* fall through */
        default:
          return InputManager::BTN_UP;
      }
    case Button::PageForward:
      switch (sideLayout) {
        case CrossPointSettings::NEXT_PREV:
          return InputManager::BTN_UP;
        case CrossPointSettings::PREV_NEXT:
          /* fall through */
        default:
          return InputManager::BTN_DOWN;
      }
  }

  return InputManager::BTN_BACK;
}

bool MappedInputManager::wasPressed(const Button button) const { return inputManager.wasPressed(mapButton(button)); }

bool MappedInputManager::wasReleased(const Button button) const { return inputManager.wasReleased(mapButton(button)); }

bool MappedInputManager::isPressed(const Button button) const { return inputManager.isPressed(mapButton(button)); }

bool MappedInputManager::wasAnyPressed() const { return inputManager.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return inputManager.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return inputManager.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  const auto layout = static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(SETTINGS.frontButtonLayout);

  switch (layout) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      return {previous, next, back, confirm};
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      return {previous, back, confirm, next};
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      return {back, confirm, previous, next};
  }
}
