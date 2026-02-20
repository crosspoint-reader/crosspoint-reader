#include "MappedInputManager.h"

#include "CrossPointSettings.h"
#include "activities/Activity.h"
extern Activity* currentActivity;

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
}  // namespace

// Given a raw button index, find the corresponding "rotated" index,
// considering display orientation and orientation support
static uint8_t mapOrientation(uint8_t halButtonIndex)
{
  if (SETTINGS.autoBtnOrientation == CrossPointSettings::AUTO_BUTTON_ORIENTATION::NO_BUTTONS ||
      !currentActivity->supportsOrientation()) {
    return halButtonIndex;
  }

  // Following "right or down for next" and "left or up for previous"
  bool rotateSide = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CW) ||
    (SETTINGS.orientation == CrossPointSettings::ORIENTATION::INVERTED);
  bool rotateFront = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::INVERTED) ||
    (SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW);

  if (rotateSide) {
    if (halButtonIndex == HalGPIO::BTN_UP) return HalGPIO::BTN_DOWN;
    if (halButtonIndex == HalGPIO::BTN_DOWN) return HalGPIO::BTN_UP;
  }

  if (!rotateFront) {
    return halButtonIndex;
  }

  // Flip the entire front cluster
  if (SETTINGS.autoBtnOrientation == CrossPointSettings::AUTO_BUTTON_ORIENTATION::SIDE_AND_FRONT_ALL)
  {
    switch (halButtonIndex) {
      case HalGPIO::BTN_BACK: return HalGPIO::BTN_RIGHT;
      case HalGPIO::BTN_CONFIRM: return HalGPIO::BTN_LEFT;
      case HalGPIO::BTN_LEFT: return HalGPIO::BTN_CONFIRM;
      case HalGPIO::BTN_RIGHT: return HalGPIO::BTN_BACK;
    }
  }

  // Flip only whatever buttons have been configured for navigation,
  // physical confirm and back remain fixed
  if (SETTINGS.autoBtnOrientation == CrossPointSettings::AUTO_BUTTON_ORIENTATION::SIDE_AND_FRONT_NAV)
  {
    if (halButtonIndex == SETTINGS.frontButtonLeft) return SETTINGS.frontButtonRight;
    if (halButtonIndex == SETTINGS.frontButtonRight) return SETTINGS.frontButtonLeft;
  }

  return halButtonIndex;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(mapOrientation(SETTINGS.frontButtonBack));
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(mapOrientation(SETTINGS.frontButtonConfirm));
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(mapOrientation(SETTINGS.frontButtonLeft));
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(mapOrientation(SETTINGS.frontButtonRight));
    case Button::Up:
      // Side buttons remain fixed for Up/Down, except with auto button orientation
      return (gpio.*fn)(mapOrientation(HalGPIO::BTN_UP));
    case Button::Down:
      // Side buttons remain fixed for Up/Down, except with auto button orientation
      return (gpio.*fn)(mapOrientation(HalGPIO::BTN_DOWN));
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(mapOrientation(side.pageBack));
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(mapOrientation(side.pageForward));
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == mapOrientation(SETTINGS.frontButtonBack)) {
      return back;
    }
    if (hw == mapOrientation(SETTINGS.frontButtonConfirm)) {
      return confirm;
    }
    if (hw == mapOrientation(SETTINGS.frontButtonLeft)) {
      return previous;
    }
    if (hw == mapOrientation(SETTINGS.frontButtonRight)) {
      return next;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}