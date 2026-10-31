#pragma once

#include <cstdint>

// Main-loop-owned snapshots. The renderer never reads or mutates this queue.
class ButtonInputBuffer {
 public:
  struct Frame {
    uint32_t heldMs = 0;
    uint8_t pressed = 0;
    uint8_t released = 0;
    uint8_t held = 0;

    bool navigationOnly(uint8_t mask) const {
      const uint8_t buttons = pressed | released | held;
      return buttons != 0 && (buttons & ~mask) == 0;
    }
  };

  static constexpr uint8_t CAPACITY = 32;

  bool capture(Frame frame, bool enqueue = true) {
    blockedThisSample = suppressed;
    suppressed &= frame.held;
    frame.pressed &= ~blockedThisSample;
    frame.released &= ~blockedThisSample;
    frame.held &= ~blockedThisSample;
    live = frame;
    if (!enqueue) {
      // Synchronous input pumps own their events; stale menu clicks cannot resume after them.
      clear();
      overflowed = false;
      return true;
    }
    if (overflowed) {
      live = {};
      if (frame.held == 0) overflowed = false;
      return true;
    }
    if ((frame.pressed | frame.released) == 0) return true;
    if (count == CAPACITY) {
      // A partial history could activate the wrong row. Drop it and wait for idle.
      clear();
      live = {};
      overflowed = true;
      return false;
    }
    frames[(head + count) % CAPACITY] = frame;
    ++count;
    return true;
  }

  bool pop(Frame& frame) {
    if (count == 0) return false;
    frame = frames[head];
    head = (head + 1) % CAPACITY;
    --count;
    return true;
  }

  const Frame* front() const { return count == 0 ? nullptr : &frames[head]; }
  const Frame& current() const { return live; }
  uint8_t blocked() const { return blockedThisSample; }
  bool dropping() const { return overflowed; }

  void clear() { head = count = 0; }

  void reset(uint8_t held) {
    clear();
    live = {};
    suppressed = blockedThisSample = held;
    overflowed = false;
  }

 private:
  Frame frames[CAPACITY]{};
  Frame live{};
  uint8_t head = 0;
  uint8_t count = 0;
  uint8_t suppressed = 0;
  uint8_t blockedThisSample = 0;
  bool overflowed = false;
};
