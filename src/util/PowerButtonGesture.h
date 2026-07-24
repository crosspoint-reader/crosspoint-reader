#pragma once

#include <cstdint>

class PowerButtonGesture {
 public:
  enum class Event : uint8_t { None, Single, Double, Hold };

  static constexpr uint32_t DOUBLE_CLICK_MS = 350;
  static constexpr uint32_t HOLD_MS = 500;

  Event update(const uint32_t now, const bool pressed, const bool released, const bool isPressed,
               const uint32_t heldMs, const bool doubleClickEnabled, const bool holdEnabled = true) {
    if (!doubleClickEnabled && waitingForSecond_) {
      waitingForSecond_ = false;
      secondPressed_ = false;
    }

    if (pressed) {
      holdHandled_ = false;
      if (doubleClickEnabled && waitingForSecond_ && elapsed(now, firstReleaseAt_) <= DOUBLE_CLICK_MS) {
        waitingForSecond_ = false;
        secondPressed_ = true;
      }
    }

    if (holdEnabled && isPressed && heldMs >= HOLD_MS && !holdHandled_) {
      holdHandled_ = true;
      waitingForSecond_ = false;
      secondPressed_ = false;
      return Event::Hold;
    }

    if (released) {
      if (holdHandled_) {
        holdHandled_ = false;
        secondPressed_ = false;
        return Event::None;
      }
      if (doubleClickEnabled) {
        if (secondPressed_) {
          secondPressed_ = false;
          return Event::Double;
        }
        waitingForSecond_ = true;
        firstReleaseAt_ = now;
        return Event::None;
      }
      return Event::Single;
    }

    if (doubleClickEnabled && waitingForSecond_ && elapsed(now, firstReleaseAt_) >= DOUBLE_CLICK_MS) {
      waitingForSecond_ = false;
      return Event::Single;
    }
    return Event::None;
  }

  void cancel() {
    waitingForSecond_ = false;
    secondPressed_ = false;
    holdHandled_ = false;
  }

 private:
  static constexpr uint32_t elapsed(const uint32_t now, const uint32_t since) { return now - since; }

  uint32_t firstReleaseAt_ = 0;
  bool waitingForSecond_ = false;
  bool secondPressed_ = false;
  bool holdHandled_ = false;
};
