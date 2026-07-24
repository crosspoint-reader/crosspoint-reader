#pragma once

#include <array>
#include <cstddef>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };

  std::array<bool, 11> pressed{};
  std::array<bool, 11> released{};
  std::array<bool, 11> held{};
  unsigned long heldTime = 0;
  bool swapped = false;

  bool wasPressed(const Button button) const { return pressed[index(button)]; }
  bool wasReleased(const Button button) const { return released[index(button)]; }
  bool isPressed(const Button button) const { return held[index(button)]; }
  unsigned long getHeldTime() const { return heldTime; }
  bool isNavDirectionSwapped() const { return swapped; }

  void clearEdges() {
    pressed.fill(false);
    released.fill(false);
  }

 private:
  static constexpr size_t index(const Button button) { return static_cast<size_t>(button); }
};
