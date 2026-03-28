#pragma once

#include <HalGPIO.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  void update() const;
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // Returns true exactly once when button has been held for thresholdMs.
  // Subsequent calls return false until the button is released and pressed again.
  bool wasLongPressed(Button button, unsigned long thresholdMs) const;

  // Returns true if wasLongPressed already fired for button (remains true until next update after release).
  // Use this to suppress the release event that follows a long press.
  bool isLongPressHandled(Button button) const;

  // Returns true exactly once when button is pressed a second time within windowMs of the first press.
  // The first press arms the detector and returns false. Subsequent calls return false until the
  // button is released (doublePressConsumed_ cleared in update()).
  bool wasDoublePressed(Button button, unsigned long windowMs) const;

  // Returns true if wasDoublePressed already fired for button (remains true until next update after
  // release). Use this to suppress the release event that follows a double press in release-mode
  // page turning.
  bool isDoublePressHandled(Button button) const;

  // Returns true while the double press detector is armed for button (first press received, waiting
  // for second press within the window). Use this to suppress the first-release page turn so it is
  // not fired before knowing whether a second press will arrive.
  bool isDoublePressArmed(Button button) const;

  // Returns true exactly once when the double press window expires without a second press arriving.
  // Use this to fire the page turn that was deferred (suppressed) during the armed window.
  bool wasDoublePressExpired(Button button) const;

 private:
  HalGPIO& gpio;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;

  // Long press state
  mutable bool longPressFired_ = false;
  mutable bool pendingLongPressReset_ = false;
  mutable uint8_t longPressButton_ = 0;

  // Double press state
  mutable bool doublePressArmed_ = false;
  mutable bool doublePressConsumed_ = false;
  mutable bool doublePressExpired_ = false;
  mutable uint8_t doublePressButton_ = 0;
  mutable uint8_t doublePressExpiredButton_ = 0;
  mutable unsigned long doublePressFirstMs_ = 0;
  mutable unsigned long doublePressWindowMs_ = 0;
  // Timestamp of the last fired double press; blocks re-arming for one window duration so a
  // post-skip page-turn press cannot immediately start another double press interaction.
  mutable unsigned long doublePressLastFiredMs_ = 0;
};
