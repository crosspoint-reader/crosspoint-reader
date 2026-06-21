#include "MappedInputManager.h"

#include <FreeInkUI.h>
#include <GfxRenderer.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/TouchRegistry.h"

bool MappedInputManager::isNavDirectionSwapped() const {
  // Key the swap on the orientation the screen is *actually* rendered at, not the persisted reader
  // setting. The reader (and its modal menus) render rotated, so navigation/labels flip there; the
  // home and settings UI render in portrait, so they never flip even when a rotated reader is configured.
  const auto orientation = renderer.getOrientation();
  return SETTINGS.frontButtonFollowOrientation &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

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
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW (frontButtonFollowOrientation) so it matches the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                                     : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                                     : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
  }

  return false;
}

// Top-left corner fallback, as a fraction of the logical screen. Generous to hit.
static constexpr float BACK_GESTURE_FRAC_X = 0.22f;
static constexpr float BACK_GESTURE_FRAC_Y = 0.12f;
static constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.14f;
static constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
static constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasBottomEdgeSwipeUp() const {
  float nxs = 0.0f, nys = 0.0f, nxe = 0.0f, nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;

  int sx = 0, sy = 0, ex = 0, ey = 0;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);

  const int screenHeight = renderer.getScreenHeight();
  const int bottomEdgeTop = screenHeight - static_cast<int>(screenHeight * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
  const bool isBackSwipe = sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (isBackSwipe) rememberTouchHeldTime();
  return isBackSwipe;
}

bool MappedInputManager::wasBackGesture() const {
  if (wasBottomEdgeSwipeUp()) return true;

  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  // A tap on the theme's header Back target acts as Back.
  int id = 0;
  if (TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Back, id)) {
    rememberTouchHeldTime();
    return true;
  }
  // Else the top-left corner, for screens with no Back target (e.g. the reader).
  const bool isTopLeftBack = lx <= renderer.getScreenWidth() * BACK_GESTURE_FRAC_X &&
                             ly <= renderer.getScreenHeight() * BACK_GESTURE_FRAC_Y;
  if (isTopLeftBack) rememberTouchHeldTime();
  return isTopLeftBack;
}

bool MappedInputManager::wasItemTapped(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  const bool hit = TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Item, id);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasItemTouchedDown(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) {
    touchSelectTracking = false;
    touchSelectEmitted = false;
    touchSelectId = -1;
    return false;
  }

  if (!touchSelectTracking) {
    float downNx = 0.0f, downNy = 0.0f;
    if (!gpio.wasTouchDown(downNx, downNy)) return false;

    int lx = 0, ly = 0;
    renderer.tapToLogical(downNx, downNy, lx, ly);
    int candidateId = -1;
    if (!TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Item, candidateId)) {
      touchSelectTracking = false;
      touchSelectEmitted = false;
      touchSelectId = -1;
      return false;
    }
    touchSelectTracking = true;
    touchSelectEmitted = false;
    touchSelectId = candidateId;
  }

  if (touchSelectEmitted || heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  touchSelectEmitted = true;
  id = touchSelectId;
  return true;
}

bool MappedInputManager::wasItemLongPressed(int& id) const {
  static constexpr unsigned long TOUCH_LONG_PRESS_MS = 500;
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;  // release frame
  if (gpio.lastTouchHeldMs() < TOUCH_LONG_PRESS_MS) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  const bool hit = TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Item, id);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasTabTapped(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  const bool hit = TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Tab, id);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasCoverTapped(int& id) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int lx = 0, ly = 0;
  renderer.tapToLogical(nx, ny, lx, ly);
  const bool hit = TouchRegistry::getInstance().hitTest(lx, ly, TouchRegistry::Cover, id);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f, ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasListScroll(int& index, int count, int pageItems) const {
  if (count <= 0) return false;
  if (pageItems < 1) pageItems = 1;
  if (wasBottomEdgeSwipeUp()) return false;
  const SwipeDir swipe = wasSwipe();
  if (swipe == SwipeDir::Up) {
    return freeink::ui::listPageIndex(index, +1, count, pageItems);
  }
  if (swipe == SwipeDir::Down) {
    return freeink::ui::listPageIndex(index, -1, count, pageItems);
  }
  return false;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  if (wasBottomEdgeSwipeUp()) return SwipeDir::None;

  float nxs = 0.0f, nys = 0.0f, nxe = 0.0f, nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return SwipeDir::None;
  // Map both endpoints into the logical frame so the direction follows what the
  // user sees regardless of panel mount/orientation.
  int sx = 0, sy = 0, ex = 0, ey = 0;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
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

unsigned long MappedInputManager::getHeldTime() const {
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
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
