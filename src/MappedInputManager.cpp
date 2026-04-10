#include "MappedInputManager.h"

#include <Arduino.h>

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

void MappedInputManager::update() const {
  // Deferred reset: clear the long-press flag one frame after the button was released.
  // This keeps isLongPressHandled() true during the release frame so the release event
  // is suppressed, then clears it so the next press starts fresh.
  if (pendingLongPressReset_) {
    longPressFired_ = false;
    pendingLongPressReset_ = false;
  }
  gpio.update();
  if (longPressFired_ && wasReleased(static_cast<Button>(longPressButton_))) {
    pendingLongPressReset_ = true;
  }

  // Reset double press consumed state once the tracked button is released.
  // This re-enables detection for the next double press interaction.
  if (doublePressConsumed_ && wasReleased(static_cast<Button>(doublePressButton_))) {
    doublePressConsumed_ = false;
  }

  // If the double press window expires without a second press, disarm and raise the expired flag.
  // The activity reads wasDoublePressExpired() to fire the page turn that was deferred during the
  // armed window.
  if (doublePressArmed_ && !doublePressConsumed_ && doublePressWindowMs_ > 0 &&
      (millis() - doublePressFirstMs_) > doublePressWindowMs_) {
    doublePressArmed_ = false;
    doublePressExpired_ = true;
    doublePressExpiredButton_ = doublePressButton_;
  }
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

bool MappedInputManager::wasLongPressed(const Button button, const unsigned long thresholdMs) const {
  if (longPressFired_) return false;
  if (!isPressed(button)) return false;
  if (getHeldTime() < thresholdMs) return false;
  longPressFired_ = true;
  longPressButton_ = static_cast<uint8_t>(button);
  // Disarm any pending double press for this button so a long hold followed by a quick
  // second press cannot spuriously trigger wasDoublePressed in the same interaction.
  // Also stamp doublePressLastFiredMs_ so the cooldown blocks re-arming immediately after
  // a long press fires (prevents a post-long-press tap from starting a new double press window).
  if (doublePressButton_ == static_cast<uint8_t>(button)) {
    doublePressArmed_ = false;
    doublePressConsumed_ = false;
    doublePressExpired_ = false;
    doublePressLastFiredMs_ = millis();
  }
  return true;
}

bool MappedInputManager::isLongPressHandled(const Button button) const {
  return longPressFired_ && longPressButton_ == static_cast<uint8_t>(button);
}

bool MappedInputManager::wasDoublePressed(const Button button, const unsigned long windowMs) const {
  // Already consumed this double press: suppress until the button is released.
  // update() clears doublePressConsumed_ one frame after release.
  if (doublePressConsumed_ && doublePressButton_ == static_cast<uint8_t>(button)) {
    return false;
  }

  if (!wasPressed(button)) {
    return false;
  }

  const unsigned long now = millis();
  const uint8_t btnIndex = static_cast<uint8_t>(button);

  if (doublePressArmed_ && doublePressButton_ == btnIndex && (now - doublePressFirstMs_) <= windowMs) {
    // Second press arrived within the window: fire double press.
    doublePressArmed_ = false;
    doublePressConsumed_ = true;
    doublePressLastFiredMs_ = now;
    // Prevent the second press from also triggering a long press if held.
    longPressFired_ = true;
    longPressButton_ = btnIndex;
    return true;
  }

  // Block re-arming within one window duration of the last fired double press.
  // This prevents the first post-skip page-turn press from immediately starting
  // a new double press interaction that could fire another page skip.
  if (doublePressLastFiredMs_ > 0 && (now - doublePressLastFiredMs_) <= windowMs) {
    return false;
  }

  // First press, expired window, or different button: arm for the next press.
  doublePressArmed_ = true;
  doublePressButton_ = btnIndex;
  doublePressFirstMs_ = now;
  doublePressWindowMs_ = windowMs;
  doublePressExpired_ = false;  // discard any stale expired state from a previous interaction
  return false;
}

bool MappedInputManager::isDoublePressHandled(const Button button) const {
  return doublePressConsumed_ && doublePressButton_ == static_cast<uint8_t>(button);
}

bool MappedInputManager::isDoublePressArmed(const Button button) const {
  return doublePressArmed_ && doublePressButton_ == static_cast<uint8_t>(button);
}

bool MappedInputManager::wasDoublePressExpired(const Button button) const {
  if (doublePressExpired_ && doublePressExpiredButton_ == static_cast<uint8_t>(button)) {
    doublePressExpired_ = false;
    return true;
  }
  return false;
}

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