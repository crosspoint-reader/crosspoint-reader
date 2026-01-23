#include "MappedInputManager.h"

#include "CrossPointSettings.h"

decltype(InputManager::BTN_BACK) MappedInputManager::mapButton(const Button button) const {
  const auto frontLayout = static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(SETTINGS.frontButtonLayout);
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);

  switch (button) {
    case Button::Back:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          return HalGPIO::BTN_LEFT;
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          return HalGPIO::BTN_CONFIRM;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
        default:
          return HalGPIO::BTN_BACK;
      }
    case Button::Confirm:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          return HalGPIO::BTN_RIGHT;
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          return HalGPIO::BTN_LEFT;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
        default:
          return HalGPIO::BTN_CONFIRM;
      }
    case Button::Left:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
          return HalGPIO::BTN_BACK;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
        default:
          return HalGPIO::BTN_LEFT;
      }
    case Button::Right:
      switch (frontLayout) {
        case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
          return HalGPIO::BTN_CONFIRM;
        case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
        case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
        default:
          return HalGPIO::BTN_RIGHT;
      }
    case Button::Up:
      return HalGPIO::BTN_UP;
    case Button::Down:
      return HalGPIO::BTN_DOWN;
    case Button::Power:
      return HalGPIO::BTN_POWER;
    case Button::PageBack:
      switch (sideLayout) {
        case CrossPointSettings::NEXT_PREV:
          return HalGPIO::BTN_DOWN;
        case CrossPointSettings::PREV_NEXT:
        default:
          return HalGPIO::BTN_UP;
      }
    case Button::PageForward:
      switch (sideLayout) {
        case CrossPointSettings::NEXT_PREV:
          return HalGPIO::BTN_UP;
        case CrossPointSettings::PREV_NEXT:
        default:
          return HalGPIO::BTN_DOWN;
      }
  }

  return HalGPIO::BTN_BACK;
}

bool MappedInputManager::wasPressed(const Button button) const { return gpio.wasPressed(mapButton(button)); }

bool MappedInputManager::wasReleased(const Button button) const { return gpio.wasReleased(mapButton(button)); }

bool MappedInputManager::isPressed(const Button button) const { return gpio.isPressed(mapButton(button)); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

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
