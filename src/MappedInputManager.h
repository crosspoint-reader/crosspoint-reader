#pragma once

#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  // Touch "back" gesture: a tap on the theme's header Back target, or in the
  // top-left corner. Folded into Back's edges, so every screen gets it for free.
  bool wasBackGesture() const;
  // True (and writes the id) if a tap this frame hit a TouchRegistry item.
  // Activities treat the id as "select + activate". False on non-touch devices.
  bool wasItemTapped(int& id) const;
  // Press-edge of wasItemTapped: fires on touch-DOWN over an item so the activity
  // can show it selected before release. Mirrors button nav (move, then confirm).
  bool wasItemTouchedDown(int& id) const;
  // Subset of wasItemTapped's releases held past the long-press threshold (check
  // this first). Distinguishes tap vs press-and-hold.
  bool wasItemLongPressed(int& id) const;
  // wasItemTapped for tab-bar tabs (id = tab index) and cover/card targets
  // (id = item index). Distinct kinds so a screen with both doesn't confuse them.
  bool wasTabTapped(int& id) const;
  bool wasCoverTapped(int& id) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  GfxRenderer& renderer;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
