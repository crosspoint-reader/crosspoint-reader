#pragma once

#include <Arduino.h>

#include "MappedInputManager.h"

// Forward declaration for the global accessor used by Activity.h.
// Defined in main.cpp alongside the ButtonEventManager instance.
class ButtonEventManager;
ButtonEventManager& globalButtonEvents();

// Classifies raw button edges into Short, Double, and Long press events.
//
// Per-button state machines run each loop() tick. The key latency rule:
//   - If no double-click action is configured for a button, Short fires immediately
//     on release (zero extra wait).
//   - If a double-click action IS configured, Short is delayed by DOUBLE_WINDOW_MS
//     to allow disambiguation.
//   - Long fires as soon as hold time >= LONG_PRESS_MS; release is not required.
//   - Double fires on the second release within DOUBLE_WINDOW_MS.
//
// Activities query consumeEvent() each loop tick to receive pending events.
// drain() resets all state machines — call it on activity transitions.

class ButtonEventManager {
 public:
  using Button = MappedInputManager::Button;

  enum class PressType { Short, Double, Long };

  struct ButtonEvent {
    Button button;
    PressType type;
  };

  // Timing constants (milliseconds)
  static constexpr unsigned long LONG_PRESS_MS = 1000;
  static constexpr unsigned long DOUBLE_WINDOW_MS = 300;

  explicit ButtonEventManager(MappedInputManager& input) : input(input) {}

  // Call once per main loop tick, after MappedInputManager::update().
  void update();

  // Returns the next pending event, or false if none. Call repeatedly until
  // false to drain all events for this tick.
  bool consumeEvent(ButtonEvent& out);

  // Reset all per-button FSMs. Call on activity transitions to prevent bleed-through.
  void drain();

  // Preserve a default event for activity processing after main loop dispatch.
  // This is used when the configured action is BTN_DEFAULT.
  void pushEventFront(Button button, PressType type);

  // Returns true if a double-click action is configured for this button.
  // ButtonEventManager queries CrossPointSettings internally.
  static bool hasDoubleAction(Button button);

 private:
  static constexpr int NUM_BUTTONS = 7;
  static constexpr Button ALL_BUTTONS[NUM_BUTTONS] = {
      Button::Back, Button::Confirm, Button::Left, Button::Right, Button::PageBack, Button::PageForward, Button::Power,
  };

  enum class State { Idle, Pressed, ReleasedOnce, DoublePressed };

  struct PerButton {
    State state = State::Idle;
    unsigned long pressDownTime = 0;  // when the current (or first) press started
    unsigned long releaseTime = 0;    // when the first release happened (for double-click window)
  };

  PerButton buttons[NUM_BUTTONS];

  // Pending events ring buffer (small — at most one event per button per tick)
  static constexpr int EVENT_BUF = 16;
  ButtonEvent eventBuf[EVENT_BUF] = {};
  int eventHead = 0;
  int eventTail = 0;

  MappedInputManager& input;

  void pushEvent(Button button, PressType type);
  void processButton(int idx, Button btn);
};
