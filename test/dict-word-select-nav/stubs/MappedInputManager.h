#pragma once
// Host-test stub for src/MappedInputManager.h.
// Replaces the full device-bound header with only the surface area that
// WordSelectNavigator.cpp touches.

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  // Test-controlled state setters.
  void setReleased(Button btn, bool released) { released_[static_cast<int>(btn)] = released; }
  void setPressed(Button btn, bool pressed) { pressed_[static_cast<int>(btn)] = pressed; }
  void setHeldTime(unsigned long ms) { heldTime_ = ms; }
  void reset() {
    for (int i = 0; i < 9; ++i) {
      released_[i] = false;
      pressed_[i] = false;
    }
    heldTime_ = 0;
  }

  bool wasReleased(Button button) const { return released_[static_cast<int>(button)]; }
  bool isPressed(Button button) const { return pressed_[static_cast<int>(button)]; }
  unsigned long getHeldTime() const { return heldTime_; }

 private:
  bool released_[9] = {false};
  bool pressed_[9] = {false};
  unsigned long heldTime_ = 0;
};
