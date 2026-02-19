#include "MappedInputManager.h"

#include "CrossPointSettings.h"

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

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

void MappedInputManager::update() {
  gpio.update();

  syntheticConfirmPress = false;
  syntheticConfirmRelease = false;
  syntheticBackPress = false;
  syntheticBackRelease = false;

  const bool confirmBackMode = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::CONFIRM_BACK;
  if (!confirmBackMode) {
    powerSinglePending = false;
    powerFirstReleaseTime = 0;
    ignoreNextPowerRelease = false;
    return;
  }

  const unsigned long now = millis();

  if (powerSinglePending && powerFirstReleaseTime > 0 && (now - powerFirstReleaseTime) >= DOUBLE_CLICK_MS) {
    syntheticConfirmPress = true;
    syntheticConfirmRelease = true;
    powerSinglePending = false;
    powerFirstReleaseTime = 0;
  }

  if (gpio.wasPressed(HalGPIO::BTN_POWER) && powerSinglePending && powerFirstReleaseTime > 0 &&
      (now - powerFirstReleaseTime) < DOUBLE_CLICK_MS) {
    syntheticBackPress = true;
    syntheticBackRelease = true;
    powerSinglePending = false;
    powerFirstReleaseTime = 0;
    ignoreNextPowerRelease = true;
  }

  if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
    if (ignoreNextPowerRelease) {
      ignoreNextPowerRelease = false;
    } else {
      powerSinglePending = true;
      powerFirstReleaseTime = now;
    }
  }
}

bool MappedInputManager::wasPressed(const Button button) const {
  const bool wasPressedRaw = mapButton(button, &HalGPIO::wasPressed);

  if (SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::CONFIRM_BACK) {
    return wasPressedRaw;
  }

  if (button == Button::Confirm) {
    return wasPressedRaw || syntheticConfirmPress;
  }
  if (button == Button::Back) {
    return wasPressedRaw || syntheticBackPress;
  }
  return wasPressedRaw;
}

bool MappedInputManager::wasReleased(const Button button) const {
  const bool wasReleasedRaw = mapButton(button, &HalGPIO::wasReleased);

  if (SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::CONFIRM_BACK) {
    return wasReleasedRaw;
  }

  if (button == Button::Confirm) {
    return wasReleasedRaw || syntheticConfirmRelease;
  }
  if (button == Button::Back) {
    return wasReleasedRaw || syntheticBackRelease;
  }
  return wasReleasedRaw;
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return previous;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return next;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
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
