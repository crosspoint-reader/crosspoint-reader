#pragma once

#include <Arduino.h>

#include "MappedInputManager.h"

// Forward declaration for the global accessor used by Activity.h.
// Defined in main.cpp alongside the ButtonEventManager instance.
class ButtonEventManager;
ButtonEventManager& globalButtonEvents();

// Classifies raw button edges into Short, Double, and Long press events.
//
// Edges are produced by the background sampler (HalGPIO) and queued with the
// millis() timestamp at which they were detected. update() drains that queue and
// drives a per-button state machine from each discrete edge, so a complete tap
// that began and ended while the loop task was busy (a single drained edge pair)
// is still classified correctly. The key latency rule:
//   - If no double-click action is configured for a button, Short fires immediately
//     on release (zero extra wait).
//   - If a double-click action IS configured, Short is delayed by DOUBLE_WINDOW_MS
//     to allow disambiguation.
//   - Long fires as soon as hold time >= LONG_PRESS_MS; release is not required.
//   - Double fires on the second release within DOUBLE_WINDOW_MS.
//
// Activities query consumeEvent() each loop tick to receive pending events.
// drain() resets all state machines — call it on activity transitions.
//
// Aliased buttons: Up/PageBack and Down/PageForward are two logical names for the
// SAME physical side button (see MappedInputManager::rawIndex). Each name runs its
// own FSM, so one press emits one event per name — by design, so list-style
// activities can match Up/Down while readers match PageBack/PageForward. An activity
// must therefore accept only ONE name per pair in a given condition, or it will
// handle the same press twice.

class ButtonEventManager {
 public:
  using Button = MappedInputManager::Button;

  enum class PressType { Short, Double, Long };

  struct ButtonEvent {
    Button button;
    PressType type;
    // When the key went down for this gesture, on the sampler's clock (the second press of a
    // Double). A list times a double-tap from it: the loop may handle the event much later.
    unsigned long pressMs = 0;
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

  // Temporarily force double-click detection for a button (adds latency to Short press).
  // Call this in the Activity's transition setup or loop.
  void forceDoubleAction(Button button, bool enable = true) {
    if (enable) {
      forcedDoubleMask |= (1 << static_cast<int>(button));
    } else {
      forcedDoubleMask &= ~(1 << static_cast<int>(button));
    }
  }

  // Preserve a default event for activity processing after main loop dispatch.
  // This is used when the configured action is BTN_DEFAULT.
  void pushEventFront(const ButtonEvent& event);

  // Returns true while a button's first release is waiting for the
  // double-click decision window to expire (i.e. a Short is pending).
  bool isShortPending(Button button) const;

  // True while any button is mid-gesture -- held down, or released once and waiting out the
  // double-click window -- or an emitted event has not been consumed yet. Long uninterruptible
  // work on the loop task must not start then: the abort hook (CooperativeAbort) fires on NEW
  // edges, and a gesture whose edges were already drained only becomes an event in a later
  // update() on this same task, so the work would sit on it until it finished. Seen on the X3:
  // a page-turn tap drained inside the double-click window was held for a whole 1.5-5 s image
  // decode ("next turn 18 ms after the decode ended").
  bool isGestureInFlight() const;
  // The first half of that: a button is held, or released once and waiting out the double-click
  // window, so an event is still to come. Unlike isGestureInFlight() it ignores emitted events no
  // one consumed, which stay queued on a screen that reads presses another way (see the
  // pushEventFront() note in main.cpp) -- the serial CMD:IDLE would otherwise wait on them forever.
  bool isMidGesture() const;

  // Returns true if a double-click action is configured for this button.
  // ButtonEventManager queries CrossPointSettings internally. Answers identically for
  // both names of an aliased pair, so one physical button always has one wait policy.
  bool hasDoubleAction(Button button) const;

  // Whether a reader activity is on top. Set once from main.cpp; until it is, every configured
  // double action is assumed to apply and its key waits out the double-click window.
  using ReaderOnTopQuery = bool (*)();
  void setReaderOnTopQuery(ReaderOnTopQuery query) { readerOnTopQuery = query; }

  // Raw record of a button's press-down edges, for code that classifies taps itself instead of
  // consuming Short/Double events — ButtonNavigator's list paging, which must keep firing on the
  // press edge and so cannot wait out a double-click window.
  //
  // All three come from the background sampler: `count` rises once per debounced press-down
  // edge even when several land inside one loop tick, and the timestamps are when the sampler saw
  // the presses, not when the loop got round to them. Measuring a double-tap any other way means
  // measuring the redraw that happened in between — an e-ink refresh alone outlasts the window.
  struct PressLog {
    uint16_t count = 0;              // press-down edges since the last drain()
    unsigned long lastPressMs = 0;   // most recent press-down
    unsigned long priorPressMs = 0;  // the one before it
  };
  [[nodiscard]] PressLog pressLog(Button button) const;

 private:
  static constexpr int NUM_BUTTONS = 9;
  static constexpr Button ALL_BUTTONS[NUM_BUTTONS] = {
      Button::Back, Button::Confirm,  Button::Left,        Button::Right, Button::Up,
      Button::Down, Button::PageBack, Button::PageForward, Button::Power,
  };

  uint32_t forcedDoubleMask = 0;

  ReaderOnTopQuery readerOnTopQuery = nullptr;

  // The double action configured for a key. Up/Down answer with the PageBack/PageForward settings,
  // so both names of a physical key share one wait policy.
  static uint8_t configuredDoubleAction(Button button);

  enum class State { Idle, Pressed, ReleasedOnce, DoublePressed };

  struct PerButton {
    State state = State::Idle;
    unsigned long pressDownTime = 0;  // when the current (or first) press started
    unsigned long releaseTime = 0;    // when the first release happened (for double-click window)
    // Independent of the FSM above: every press-down edge is counted and timed, whatever state
    // the machine is in and whatever it decides to do with it. See pressLog().
    unsigned long loggedPressMs = 0;
    unsigned long priorLoggedPressMs = 0;
    uint16_t pressCount = 0;
  };

  PerButton buttons[NUM_BUTTONS];

  // Pending events ring buffer (small — at most one event per button per tick)
  static constexpr int EVENT_BUF = 16;
  ButtonEvent eventBuf[EVENT_BUF] = {};
  int eventHead = 0;
  int eventTail = 0;

  MappedInputManager& input;

  void pushEvent(Button button, PressType type, unsigned long pressMs);
  // The other logical name for the same physical button (Up<->PageBack,
  // Down<->PageForward); the button itself when it has no alias.
  static Button pairedAlias(Button button);
  // Advance one button's FSM on a discrete press/release edge captured at time t.
  void applyEdge(int idx, Button btn, bool pressed, unsigned long t);
  // Advance one button's FSM on elapsed time (long-press-while-held, double-window
  // expiry), evaluated at `now` against whether the button is currently held.
  void applyTimeout(int idx, Button btn, unsigned long now, bool heldNow);
  static int buttonToIndex(Button button);
};
