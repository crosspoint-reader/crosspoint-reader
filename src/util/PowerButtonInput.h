#pragma once

#include <cstdint>

class PowerButtonInput {
 public:
  static constexpr uint16_t LONG_PRESS_MS = 400;

  void begin(bool suppressHold, bool held) {
    state = suppressHold ? State::Suppressed : held ? State::LongOnly : State::Normal;
    shortRelease = sleepHold = consumedRelease = false;
  }

  void update(bool held, bool released, uint32_t heldMs, uint16_t sleepThresholdMs) {
    shortRelease = sleepHold = consumedRelease = false;
    if (state == State::Normal && (held || released) && heldMs > LONG_PRESS_MS) state = State::LongOnly;
    if (released || !held) {
      shortRelease = released && state == State::Normal;
      consumedRelease = released && !shortRelease;
      state = State::Normal;
      return;
    }
    sleepHold = state != State::Suppressed && heldMs > sleepThresholdMs;
  }

  void consume() {
    state = State::Suppressed;
    shortRelease = sleepHold = false;
  }

  bool wasShortReleased() const { return shortRelease; }
  bool shouldSleep() const { return sleepHold; }
  bool wasReleaseConsumed() const { return consumedRelease; }
  bool suppressesHold() const { return state == State::Suppressed; }

 private:
  enum class State : uint8_t { Suppressed, LongOnly, Normal };
  State state = State::Suppressed;
  bool shortRelease = false;
  bool sleepHold = false;
  bool consumedRelease = false;
};
