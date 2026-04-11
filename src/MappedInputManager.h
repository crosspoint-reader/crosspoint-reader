#pragma once

#include <HalGPIO.h>
#include <HalIMU.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  enum class Orientation { Portrait, PortraitInverted, LandscapeClockwise, LandscapeCounterClockwise };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  void update() {
    gpio.update();
    updateTilt();
  }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True if any tilt event fired this frame (for sleep timer reset).
  bool wasTiltActive() const { return tiltPageForward || tiltPageBack; }

  // Set the effective screen orientation (called by OrientationHelper when
  // switching activities). Button mapping uses this instead of the raw
  // SETTINGS.orientation so that UI pages in Portrait mode are not affected
  // by a landscape setting.
  void setEffectiveOrientation(Orientation o) { effectiveOrientation = o; }

 private:
  HalGPIO& gpio;
  Orientation effectiveOrientation = Orientation::Portrait;

  // Tilt page turn state machine
  enum class TiltState : uint8_t { IDLE, COOLDOWN };
  TiltState tiltState = TiltState::IDLE;
  bool tiltPageForward = false;  // One-shot event: tilt triggered PageForward
  bool tiltPageBack = false;     // One-shot event: tilt triggered PageBack
  int16_t filteredAccel = 0;     // Low-pass filtered acceleration for tilt axis

  void updateTilt();
  bool wasTiltTriggered(Button button) const;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
