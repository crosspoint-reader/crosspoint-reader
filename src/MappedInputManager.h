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
  // Reusable touch "back" gesture: a tap released in the top-left corner, OR a tap
  // on the header back area registered by the theme. Folded into Back's press/
  // release edges, so every screen gets it with no per-activity code. False on
  // non-touch devices.
  bool wasBackGesture() const;
  // One-shot: if a tap this frame hit a registered interactive element (theme
  // draw methods register them via TouchRegistry), returns true and writes the
  // element's id. Activities treat the id as "select + activate". False on
  // non-touch devices or when the tap missed every target.
  bool wasItemTapped(int& id) const;
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
