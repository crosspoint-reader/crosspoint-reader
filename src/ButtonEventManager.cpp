#include "ButtonEventManager.h"

#include "CrossPointSettings.h"

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

bool ButtonEventManager::hasDoubleAction(const Button button) const {
  if (forcedDoubleMask & (1 << static_cast<int>(button))) {
    return true;
  }
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (button) {
    case Button::Back:
      return SETTINGS.btnDoubleBack != BA::BTN_DEFAULT;
    case Button::Confirm:
      return SETTINGS.btnDoubleConfirm != BA::BTN_DEFAULT;
    case Button::Left:
      return SETTINGS.btnDoubleLeft != BA::BTN_DEFAULT;
    case Button::Right:
      return SETTINGS.btnDoubleRight != BA::BTN_DEFAULT;
    case Button::PageBack:
      return SETTINGS.btnDoublePageBack != BA::BTN_DEFAULT;
    case Button::PageForward:
      return SETTINGS.btnDoublePageForward != BA::BTN_DEFAULT;
    case Button::Power:
      return SETTINGS.btnDoublePower != BA::BTN_DEFAULT;
  }
  return false;
}

void ButtonEventManager::pushEvent(const Button button, const PressType type) {
  const int next = (eventTail + 1) % EVENT_BUF;
  if (next == eventHead) return;  // buffer full, drop oldest not possible — just drop newest
  eventBuf[eventTail] = {button, type};
  eventTail = next;
}

void ButtonEventManager::pushEventFront(const Button button, const PressType type) {
  const int prev = (eventHead - 1 + EVENT_BUF) % EVENT_BUF;
  if (prev == eventTail) return;  // buffer full
  eventHead = prev;
  eventBuf[eventHead] = {button, type};
}

bool ButtonEventManager::isShortPending(const Button button) const {
  const int idx = buttonToIndex(button);
  if (idx < 0) return false;
  return buttons[idx].state == State::ReleasedOnce;
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
  }
  eventHead = eventTail = 0;
  longPressDispatchedMask = 0;
}

void ButtonEventManager::processButton(const int idx, const Button btn) {
  PerButton& s = buttons[idx];
  const unsigned long now = millis();
  const bool pressed = input.wasPressed(btn);
  const bool released = input.wasReleased(btn);
  const bool held = input.isPressed(btn);

  switch (s.state) {
    case State::Idle:
      if (pressed) {
        s.state = State::Pressed;
        s.pressDownTime = now;
      }
      break;

    case State::Pressed:
      if (released) {
        const unsigned long heldMs = now - s.pressDownTime;
        if (heldMs >= LONG_PRESS_MS) {
          pushEvent(btn, PressType::Long);
          s.state = State::Idle;
        } else if (hasDoubleAction(btn)) {
          // Delay short-press decision until double-click window expires
          s.releaseTime = now;
          s.state = State::ReleasedOnce;
        } else {
          // No double action configured — fire immediately
          pushEvent(btn, PressType::Short);
          s.state = State::Idle;
        }
      } else if (held && now - s.pressDownTime >= LONG_PRESS_MS) {
        // Fire long press immediately once the threshold is reached.
        pushEvent(btn, PressType::Long);
        s.state = State::Idle;
      } else if (!held) {
        // Button disappeared without wasReleased edge (e.g. after drain) — reset
        s.state = State::Idle;
      }
      break;

    case State::ReleasedOnce:
      if (now - s.releaseTime >= DOUBLE_WINDOW_MS) {
        // Window expired without a second press — it was a short press.
        pushEvent(btn, PressType::Short);
        s.state = State::Idle;
        if (pressed) {
          // The user pressed again after the double-click window expired;
          // start a fresh press sequence instead of treating it as a double click.
          s.state = State::Pressed;
          s.pressDownTime = now;
        }
      } else if (pressed) {
        // Second press within window — start tracking it
        s.state = State::DoublePressed;
        s.pressDownTime = now;
      }
      break;

    case State::DoublePressed:
      if (released) {
        pushEvent(btn, PressType::Double);
        s.state = State::Idle;
      } else if (!held) {
        // Disappeared without edge — treat as double anyway
        pushEvent(btn, PressType::Double);
        s.state = State::Idle;
      }
      break;
  }
}

void ButtonEventManager::update() {
  // Clear the long-press-dispatched flag for buttons that are pressed again.
  // This allows the flag to survive through the release tick (so detectPageTurn
  // can suppress the page-turn on release) and resets cleanly on the next press.
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if ((longPressDispatchedMask & (1u << i)) && input.wasPressed(ALL_BUTTONS[i])) {
      longPressDispatchedMask &= ~(1u << i);
    }
  }
  for (int i = 0; i < NUM_BUTTONS; i++) {
    processButton(i, ALL_BUTTONS[i]);
  }
}
