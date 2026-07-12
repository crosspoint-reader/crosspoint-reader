#pragma once
#include <cstdint>
#include <string>

struct ButtonLabels {
  const char* btn1; const char* btn2; const char* btn3; const char* btn4;
};

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Up, Down, Left, Right, PageForward, PageBack, Power };

  // Simulated state for tests
  Button lastPressed = Button::Back;
  bool pressedFlag = false;
  bool releasedFlag = false;
  unsigned long heldTime = 0;

  bool wasPressed(Button b) { if (b == lastPressed && pressedFlag) { pressedFlag = false; return true; } return false; }
  bool wasReleased(Button b) { if (b == lastPressed && releasedFlag) { releasedFlag = false; return true; } return false; }
  bool isPressed(Button) { return false; }
  unsigned long getHeldTime() { return heldTime; }
  int getPressedFrontButton() { return -1; }
  void update() {}

  ButtonLabels mapLabels(const char* a, const char* b, const char* c, const char* d) {
    return {a, b, c, d};
  }

  // Test helpers
  void simulatePress(Button b) { lastPressed = b; pressedFlag = true; releasedFlag = false; }
  void simulateRelease(Button b) { lastPressed = b; releasedFlag = true; pressedFlag = false; }
};
