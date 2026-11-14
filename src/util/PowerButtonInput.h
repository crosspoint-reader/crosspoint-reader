#pragma once

#include <cstdint>

class PowerButtonInput {
 public:
  enum class Click { None, Single, Double };
  static constexpr uint32_t DOUBLE_CLICK_MS = 500;
  static constexpr uint32_t MAX_CLICK_HOLD_MS = 300;
  static constexpr uint32_t MAX_SINGLE_CLICK_HOLD_MS = 400;

  Click update(uint32_t now, bool pressed, bool released, uint32_t heldMs) {
    if ((pressed || released) && heldMs > MAX_SINGLE_CLICK_HOLD_MS) {
      reset();
      return Click::None;
    }
    if (pressed && heldMs > MAX_CLICK_HOLD_MS) {
      reset();
      return Click::None;
    }
    if (released && heldMs > MAX_CLICK_HOLD_MS) {
      reset();
      return Click::Single;
    }
    if (pending && now - clickedAt > DOUBLE_CLICK_MS) {
      pending = released;
      clickedAt = now;
      return Click::Single;
    }
    if (!released) return Click::None;
    if (pending) {
      reset();
      return Click::Double;
    }
    pending = true;
    clickedAt = now;
    return Click::None;
  }

  void reset() { pending = false; }

 private:
  bool pending = false;
  uint32_t clickedAt = 0;
};
