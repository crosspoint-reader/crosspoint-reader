#include "ButtonEventManager.h"

#include "CrossPointSettings.h"
#include "util/DoubleActionWait.h"

// Required for constexpr array out-of-class definition (C++14).
constexpr ButtonEventManager::Button ButtonEventManager::ALL_BUTTONS[ButtonEventManager::NUM_BUTTONS];

int ButtonEventManager::buttonToIndex(const Button button) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (ALL_BUTTONS[i] == button) {
      return i;
    }
  }
  return -1;
}

ButtonEventManager::Button ButtonEventManager::pairedAlias(const Button button) {
  switch (button) {
    case Button::Up:
      return Button::PageBack;
    case Button::PageBack:
      return Button::Up;
    case Button::Down:
      return Button::PageForward;
    case Button::PageForward:
      return Button::Down;
    default:
      return button;
  }
}

// Both names of an aliased pair must answer the same, because they are one physical
// button driving two FSMs. If they disagreed, one name would fire Short immediately
// while the other waited out the double-click window, and an activity accepting either
// name would act on the un-delayed event — silently defeating the configured double
// action. That is why Up/Down resolve to the PageBack/PageForward settings (there are
// no separate btnDoubleUp/Down) and why the forced mask is tested across the pair.
bool ButtonEventManager::hasDoubleAction(const Button button) const {
  const uint32_t pairMask = (1u << static_cast<int>(button)) | (1u << static_cast<int>(pairedAlias(button)));
  if (forcedDoubleMask & pairMask) {
    return true;
  }
  const uint8_t action = configuredDoubleAction(button);
  // Outside the reader a reader-only double action falls through to the screen, so there is
  // nothing to wait for (the default PAGE_BACK_10 / PAGE_FORWARD_10 on Left/Right).
  const bool readerOnTop = readerOnTopQuery == nullptr || readerOnTopQuery();
  return doubleActionNeedsWait(action != CrossPointSettings::BTN_DEFAULT,
                               CrossPointSettings::isReaderScopedAction(action), readerOnTop);
}

uint8_t ButtonEventManager::configuredDoubleAction(const Button button) {
  switch (button) {
    case Button::Back:
      return SETTINGS.btnDoubleBack;
    case Button::Confirm:
      return SETTINGS.btnDoubleConfirm;
    case Button::Left:
      return SETTINGS.btnDoubleLeft;
    case Button::Right:
      return SETTINGS.btnDoubleRight;
    case Button::Up:
    case Button::PageBack:
      return SETTINGS.btnDoublePageBack;
    case Button::Down:
    case Button::PageForward:
      return SETTINGS.btnDoublePageForward;
    case Button::Power:
      return SETTINGS.btnDoublePower;
  }
  return CrossPointSettings::BTN_DEFAULT;
}

ButtonEventManager::PressLog ButtonEventManager::pressLog(const Button button) const {
  const int idx = buttonToIndex(button);
  if (idx < 0) return {};
  const PerButton& s = buttons[idx];
  return {s.pressCount, s.loggedPressMs, s.priorLoggedPressMs};
}

void ButtonEventManager::pushEvent(const Button button, const PressType type, const unsigned long pressMs) {
  const int next = (eventTail + 1) % EVENT_BUF;
  if (next == eventHead) return;  // buffer full, drop oldest not possible — just drop newest
  eventBuf[eventTail] = {button, type, pressMs};
  eventTail = next;
}

void ButtonEventManager::pushEventFront(const ButtonEvent& event) {
  const int prev = (eventHead - 1 + EVENT_BUF) % EVENT_BUF;
  if (prev == eventTail) return;  // buffer full
  eventHead = prev;
  eventBuf[eventHead] = event;
}

bool ButtonEventManager::isShortPending(const Button button) const {
  const int idx = buttonToIndex(button);
  if (idx < 0) return false;
  return buttons[idx].state == State::ReleasedOnce;
}

bool ButtonEventManager::isGestureInFlight() const { return eventHead != eventTail || isMidGesture(); }

bool ButtonEventManager::isMidGesture() const {
  for (const auto& b : buttons) {
    if (b.state != State::Idle) return true;
  }
  return false;
}

bool ButtonEventManager::consumeEvent(ButtonEvent& out) {
  if (eventHead == eventTail) return false;
  out = eventBuf[eventHead];
  eventHead = (eventHead + 1) % EVENT_BUF;
  return true;
}

void ButtonEventManager::drain() {
  for (auto& b : buttons) {
    b.state = State::Idle;
    b.pressDownTime = 0;
    b.releaseTime = 0;
    b.loggedPressMs = 0;
    b.priorLoggedPressMs = 0;
    b.pressCount = 0;
  }
  eventHead = eventTail = 0;
  // Drop edges the sampler queued for the outgoing activity so they don't bleed in.
  input.flushRawEdges();
  // Queued touch events go the same way and for the same reason. They are not
  // button events, so this class has no other business with them — but drain()
  // is the transition hook every caller already reaches for, and a tap or pinch
  // made on the screen being left must not fire on the one being entered. Inert
  // on boards without touch.
  input.flushTouchEvents();
}

void ButtonEventManager::applyEdge(const int idx, const Button btn, const bool pressed, const unsigned long t) {
  PerButton& s = buttons[idx];
  // Count and time every press-down edge before the state machine has its say: pressLog() has to
  // answer for taps the FSM folds away (a second press inside the double window) or classifies
  // without recording (an immediate Short).
  if (pressed) {
    s.priorLoggedPressMs = s.loggedPressMs;
    s.loggedPressMs = t;
    s.pressCount++;
  }
  switch (s.state) {
    case State::Idle:
      if (pressed) {
        s.state = State::Pressed;
        s.pressDownTime = t;
      }
      break;

    case State::Pressed:
      if (!pressed) {
        const unsigned long heldMs = t - s.pressDownTime;
        if (heldMs >= LONG_PRESS_MS) {
          pushEvent(btn, PressType::Long, s.pressDownTime);
          s.state = State::Idle;
        } else if (hasDoubleAction(btn)) {
          // Delay short-press decision until double-click window expires.
          s.releaseTime = t;
          s.state = State::ReleasedOnce;
        } else {
          // No double action configured — fire immediately.
          pushEvent(btn, PressType::Short, s.pressDownTime);
          s.state = State::Idle;
        }
      }
      break;

    case State::ReleasedOnce:
      if (pressed) {
        if (t - s.releaseTime >= DOUBLE_WINDOW_MS) {
          // Window already elapsed before this press arrived (e.g. the loop task
          // was blocked past it): the first press was a Short, this starts fresh.
          pushEvent(btn, PressType::Short, s.pressDownTime);
          s.state = State::Pressed;
          s.pressDownTime = t;
        } else {
          // Second press within the window — start tracking the double.
          s.state = State::DoublePressed;
          s.pressDownTime = t;
        }
      }
      break;

    case State::DoublePressed:
      if (!pressed) {
        pushEvent(btn, PressType::Double, s.pressDownTime);
        s.state = State::Idle;
      }
      break;
  }
}

void ButtonEventManager::applyTimeout(const int idx, const Button btn, const unsigned long now, const bool heldNow) {
  PerButton& s = buttons[idx];
  switch (s.state) {
    case State::Pressed:
      if (heldNow && now - s.pressDownTime >= LONG_PRESS_MS) {
        // Fire Long as soon as the hold threshold passes, without waiting for
        // release. The eventual release edge lands in Idle and is ignored.
        pushEvent(btn, PressType::Long, s.pressDownTime);
        s.state = State::Idle;
      }
      break;

    case State::ReleasedOnce:
      if (now - s.releaseTime >= DOUBLE_WINDOW_MS) {
        // Window expired without a second press — it was a Short.
        pushEvent(btn, PressType::Short, s.pressDownTime);
        s.state = State::Idle;
      }
      break;

    default:
      break;
  }
}

void ButtonEventManager::update() {
  // 1) Replay every debounced edge the sampler captured since the last tick, in
  //    order, each with its own timestamp. Processing edges discretely (rather
  //    than sampling instantaneous wasPressed/wasReleased) means a press+release
  //    that both landed inside one slow loop iteration is still classified.
  HalGPIO::ButtonEdge edge;
  while (input.popRawEdge(edge)) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
      const Button btn = ALL_BUTTONS[i];
      if (input.rawIndex(btn) != edge.button) {
        continue;
      }
      applyEdge(i, btn, edge.pressed, edge.timeMs);
    }
  }

  // 2) Time-based transitions that no edge will deliver: long-press-while-held and
  //    double-click-window expiry.
  const unsigned long now = millis();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    applyTimeout(i, ALL_BUTTONS[i], now, input.isPressed(ALL_BUTTONS[i]));
  }
}
