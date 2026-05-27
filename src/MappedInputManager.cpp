#include "MappedInputManager.h"

#include "CrossPointSettings.h"
#include "Logging.h"

namespace {
unsigned long lastConsumedTouchPageTurnAt = 0;

bool touchPageTurnFor(const HalGPIO& gpio, const MappedInputManager::Button button) {
  if (button != MappedInputManager::Button::PageBack && button != MappedInputManager::Button::PageForward) {
    return false;
  }

  const auto touchPoint = gpio.getTouchPoint();
  if (!touchPoint.valid || touchPoint.timestamp == lastConsumedTouchPageTurnAt ||
      millis() - touchPoint.timestamp >= 1500) {
    return false;
  }

  const int touchWidth =
      BoardConfig::ACTIVE.displayWidth > BoardConfig::ACTIVE.displayHeight ? BoardConfig::ACTIVE.displayWidth
                                                                           : BoardConfig::ACTIVE.displayHeight;
  const bool prev = touchPoint.x < touchWidth / 3;
  const bool next = touchPoint.x >= (touchWidth * 2) / 3;
  const bool matched = (button == MappedInputManager::Button::PageBack && prev) ||
                       (button == MappedInputManager::Button::PageForward && next);
  if (!matched) {
    return false;
  }

  lastConsumedTouchPageTurnAt = touchPoint.timestamp;
  LOG_DBG("TOUCH", "reader side-touch x=%u y=%u width=%d button=%s", touchPoint.x, touchPoint.y, touchWidth,
          button == MappedInputManager::Button::PageBack ? "back" : "forward");
  return true;
}
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

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
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  return touchPageTurnFor(gpio, button) || mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  return touchPageTurnFor(gpio, button) || mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::isTouchPressed() const { return gpio.isTouchPressed(); }

bool MappedInputManager::wasTouchPressed() const { return gpio.wasTouchPressed(); }

bool MappedInputManager::wasTouchReleased() const { return gpio.wasTouchReleased(); }

InputManager::TouchPoint MappedInputManager::getTouchPoint() const { return gpio.getTouchPoint(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

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
      return leftLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return rightLabel;
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
