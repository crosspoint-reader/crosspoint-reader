#include "MappedInputManager.h"

#include <HalIMU.h>

#include "CrossPointSettings.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
// X4: BTN_DOWN(5) is physically UPPER, BTN_UP(4) is physically LOWER.
// X3: BTN_UP(4) is physically UPPER, BTN_DOWN(5) is physically LOWER (normal).
constexpr SideLayoutMap kSideLayoutsX4[] = {
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},  // PREV_NEXT: top(DOWN)=prev, bottom(UP)=next
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},  // NEXT_PREV
};
constexpr SideLayoutMap kSideLayoutsX3[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},  // PREV_NEXT: top(UP)=prev, bottom(DOWN)=next
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},  // NEXT_PREV
};

// Mirror a front button hardware index (0<->3, 1<->2) for inverted orientation.
// Physical buttons reverse left-to-right when the device is held upside down.
constexpr ButtonIndex mirrorFront(ButtonIndex idx) { return 3 - idx; }
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = gpio.deviceIsX3() ? kSideLayoutsX3[sideLayout] : kSideLayoutsX4[sideLayout];
  const bool inverted = effectiveOrientation == Orientation::PortraitInverted;
  const bool landscapeCW = effectiveOrientation == Orientation::LandscapeClockwise;
  const bool landscapeCCW = effectiveOrientation == Orientation::LandscapeCounterClockwise;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      // Inverted: mirror the hardware position.
      return (gpio.*fn)(inverted ? mirrorFront(SETTINGS.frontButtonBack) : SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(inverted ? mirrorFront(SETTINGS.frontButtonConfirm) : SETTINGS.frontButtonConfirm);
    case Button::Left:
      // CCW: front buttons rotate to right side, physical top-to-bottom is
      // GPIO 3,2,1,0. "Left" (previous) should be physical top = GPIO of Right.
      if (inverted) return (gpio.*fn)(mirrorFront(SETTINGS.frontButtonLeft));
      if (landscapeCCW) return (gpio.*fn)(SETTINGS.frontButtonRight);
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // CCW: "Right" (next) should be physical bottom-ish = GPIO of Left.
      if (inverted) return (gpio.*fn)(mirrorFront(SETTINGS.frontButtonRight));
      if (landscapeCCW) return (gpio.*fn)(SETTINGS.frontButtonLeft);
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      // Inverted: swap physical Up/Down.
      return (gpio.*fn)(inverted ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
    case Button::Down:
      return (gpio.*fn)(inverted ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      // Inverted: side buttons swap physical position.
      // CW: side buttons move to bottom, Down(left)/Up(right), swap needed.
      if (inverted || landscapeCW) return (gpio.*fn)(side.pageForward);
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      if (inverted || landscapeCW) return (gpio.*fn)(side.pageBack);
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  return mapButton(button, &HalGPIO::wasPressed) || wasTiltTriggered(button);
}

bool MappedInputManager::wasReleased(const Button button) const {
  return mapButton(button, &HalGPIO::wasReleased) || wasTiltTriggered(button);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  // LandscapeCCW: front buttons rotate to right side (vertical). Physical
  // top-to-bottom becomes GPIO 3,2,1,0. drawButtonHints reverses labels
  // (0<->3, 1<->2) so that visual top = labels[3]. To make physical top = previous
  // (user expectation: up = previous page), we swap previous<->next in the label
  // assignment so that after drawButtonHints' reversal the labels match.
  const bool swapPrevNext = effectiveOrientation == Orientation::LandscapeCounterClockwise;
  const char* prev = swapPrevNext ? next : previous;
  const char* nxt = swapPrevNext ? previous : next;

  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return prev;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return nxt;
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

namespace {
// QMI8658 ±2G range: 16384 LSB/G
// threshold_high = 0.4G ≈ 6554 LSB
// threshold_low  = 0.2G ≈ 3277 LSB
constexpr int16_t TILT_THRESHOLD_HIGH = 6554;
constexpr int16_t TILT_THRESHOLD_LOW = 3277;
}  // namespace

void MappedInputManager::updateTilt() {
  // Clear one-shot events from previous frame
  tiltPageForward = false;
  tiltPageBack = false;

  if (!SETTINGS.tiltPageTurn || !imu.isAvailable()) return;

  imu.update();

  // Select axis and sign based on screen orientation.
  // QMI8658 on X3 PCB: Y-axis = left/right tilt (roll), X-axis = forward/backward (pitch).
  int16_t rawAccel = 0;
  bool invertDirection = false;
  switch (effectiveOrientation) {
    case Orientation::Portrait:
      rawAccel = imu.getAccelY();
      break;
    case Orientation::PortraitInverted:
      rawAccel = imu.getAccelY();
      invertDirection = true;
      break;
    case Orientation::LandscapeClockwise:
      rawAccel = imu.getAccelX();
      break;
    case Orientation::LandscapeCounterClockwise:
      rawAccel = imu.getAccelX();
      invertDirection = true;
      break;
  }

  // Low-pass filter (EMA): filteredAccel += (raw - filteredAccel) / N
  // N=6 gives smooth response (~0.2s settling) while filtering out shakes.
  filteredAccel += (rawAccel - filteredAccel) / 6;

  const int16_t absAccel = (filteredAccel > 0) ? filteredAccel : ((filteredAccel == -32768) ? 32767 : -filteredAccel);

  switch (tiltState) {
    case TiltState::IDLE:
      if (absAccel > TILT_THRESHOLD_HIGH) {
        // Trigger page turn. Tilt direction follows sideButtonLayout setting:
        // PREV_NEXT (Standard): right tilt = forward, left tilt = back
        // NEXT_PREV (Reversed): right tilt = back, left tilt = forward
        const bool reversedLayout = SETTINGS.sideButtonLayout == CrossPointSettings::SIDE_BUTTON_LAYOUT::NEXT_PREV;
        bool forward = invertDirection ? (filteredAccel < 0) : (filteredAccel > 0);
        if (reversedLayout) forward = !forward;
        if (forward) {
          tiltPageForward = true;
        } else {
          tiltPageBack = true;
        }
        tiltState = TiltState::COOLDOWN;
      }
      break;

    case TiltState::COOLDOWN:
      // Wait until device returns to near-horizontal
      if (absAccel < TILT_THRESHOLD_LOW) {
        tiltState = TiltState::IDLE;
      }
      break;
  }
}

bool MappedInputManager::wasTiltTriggered(const Button button) const {
  if (button == Button::PageForward) return tiltPageForward;
  if (button == Button::PageBack) return tiltPageBack;
  return false;
}
