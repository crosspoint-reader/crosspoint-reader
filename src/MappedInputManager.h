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

  static constexpr unsigned long DOUBLE_CLICK_MS = 150;

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  void update();
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  mutable unsigned long powerFirstReleaseTime = 0;
  mutable bool powerSinglePending = false;
  mutable bool ignoreNextPowerRelease = false;
  mutable bool syntheticConfirmPress = false;
  mutable bool syntheticConfirmRelease = false;
  mutable bool syntheticBackPress = false;
  mutable bool syntheticBackRelease = false;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
