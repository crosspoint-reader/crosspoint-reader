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

  void update() { gpio.update(); }
  bool wasPressed(Button button);
  bool wasReleased(Button button);
  bool isPressed(Button button) const;
  // Inject a one-frame virtual activation for the given button.
  // Both wasPressed() and wasReleased() will fire for it on the next check.
  // Call from the main-loop task only (virtualActivatedMask is not thread-safe).
  void injectVirtualActivation(Button button);
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  unsigned long pendingPowerReleaseMs = 0;
  unsigned long doubleTapReadyMs = 0;
  bool pendingPowerRelease = false;
  bool doubleTapReady = false;
  bool powerReleaseConsumed = false;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  void updatePowerTapState();
  bool consumePowerConfirm();
  bool consumePowerBack();
};
