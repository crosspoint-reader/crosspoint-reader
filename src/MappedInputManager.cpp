#include "MappedInputManager.h"

#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

struct FrontLayoutMap {
  ButtonIndex back;
  ButtonIndex confirm;
  ButtonIndex left;
  ButtonIndex right;
};

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::FRONT_BUTTON_LAYOUT.
constexpr FrontLayoutMap kFrontLayouts[] = {
    {InputManager::BTN_BACK, InputManager::BTN_CONFIRM, InputManager::BTN_LEFT, InputManager::BTN_RIGHT},
    {InputManager::BTN_LEFT, InputManager::BTN_RIGHT, InputManager::BTN_BACK, InputManager::BTN_CONFIRM},
    {InputManager::BTN_CONFIRM, InputManager::BTN_LEFT, InputManager::BTN_BACK, InputManager::BTN_RIGHT},
    {InputManager::BTN_BACK, InputManager::BTN_CONFIRM, InputManager::BTN_RIGHT, InputManager::BTN_LEFT},
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {InputManager::BTN_UP, InputManager::BTN_DOWN},
    {InputManager::BTN_DOWN, InputManager::BTN_UP},
};
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (InputManager::*fn)(uint8_t) const) const {
  const auto frontLayout = static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(SETTINGS.frontButtonLayout);
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& front = kFrontLayouts[frontLayout];
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      return (inputManager.*fn)(front.back);
    case Button::Confirm:
      return (inputManager.*fn)(front.confirm);
    case Button::Left:
      return (inputManager.*fn)(front.left);
    case Button::Right:
      return (inputManager.*fn)(front.right);
    case Button::Up:
      return (inputManager.*fn)(InputManager::BTN_UP);
    case Button::Down:
      return (inputManager.*fn)(InputManager::BTN_DOWN);
    case Button::Power:
      return (inputManager.*fn)(InputManager::BTN_POWER);
    case Button::PageBack:
      return (inputManager.*fn)(side.pageBack);
    case Button::PageForward:
      return (inputManager.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &InputManager::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const {
  return mapButton(button, &InputManager::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &InputManager::isPressed); }

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
