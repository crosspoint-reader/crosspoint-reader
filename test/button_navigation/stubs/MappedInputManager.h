#pragma once

#include "util/ButtonInputBuffer.h"

extern unsigned long testNowMs;
inline unsigned long millis() { return testNowMs; }

class MappedInputManager {
 public:
  enum class Button { NavPrevious, NavNext, Left = NavPrevious, Right = NavNext };
  ButtonInputBuffer::Frame frame{};
  bool wasPressed(Button button) const { return frame.pressed & (1u << static_cast<unsigned>(button)); }
  bool wasReleased(Button button) const { return frame.released & (1u << static_cast<unsigned>(button)); }
  bool isPressed(Button button) const { return frame.held & (1u << static_cast<unsigned>(button)); }
  unsigned long getHeldTime() const { return frame.heldMs; }
};
