#include "MappedInputManager.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "components/TouchRegistry.h"

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

// Top-left corner fallback, as a fraction of the logical screen. Generous to hit.
static constexpr float BACK_GESTURE_FRAC_X = 0.22f;
static constexpr float BACK_GESTURE_FRAC_Y = 0.12f;

bool MappedInputManager::wasBackGesture() const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  // A tap on the theme's header Back target acts as Back.
  int id = 0;
  if (TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Back, id)) return true;
  // Else the top-left corner, for screens with no Back target (e.g. the reader).
  return lx <= renderer.getScreenWidth() * BACK_GESTURE_FRAC_X &&
         ly <= renderer.getScreenHeight() * BACK_GESTURE_FRAC_Y;
}

bool MappedInputManager::wasItemTapped(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  return TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Item, id);
}

bool MappedInputManager::wasItemTouchedDown(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchDown(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  return TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Item, id);
}

bool MappedInputManager::wasItemLongPressed(int& id) const {
  static constexpr unsigned long TOUCH_LONG_PRESS_MS = 500;
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;  // release frame
  if (gpio.lastTouchHeldMs() < TOUCH_LONG_PRESS_MS) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  return TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Item, id);
}

bool MappedInputManager::wasTabTapped(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  return TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Tab, id);
}

bool MappedInputManager::wasCoverTapped(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  return TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Cover, id);
}

bool MappedInputManager::wasPressed(const Button button) const {
  // A top-left tap fires on the release frame; expose it on Back's press edge too
  // so menus that act on wasPressed(Back) also respond. Deliberately NOT folded
  // into isPressed, so a quick tap never satisfies the readers' long-press-home.
  if (button == Button::Back && wasBackGesture()) return true;
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

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
