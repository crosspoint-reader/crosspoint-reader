#pragma once

#include <cstdint>

#include "QuickLockState.h"

class ButtonShortcutController {
 public:
  enum class ChordAction : uint8_t { Screenshot = 0, QuickLock = 1, NextPage = 2, PreviousPage = 3, Disabled = 4 };
  enum class Event : uint8_t { None, QuickLockChanged, Screenshot, NextPage, PreviousPage };

  struct Result {
    Event event = Event::None;
    bool consumeInput = false;
  };

  [[nodiscard]] Result update(uint32_t nowMs, bool powerPressed, bool downPressed, bool powerReleased,
                              bool quickLockOnShortPower, ChordAction chordAction) {
    if (chordActive) {
      if (!powerPressed && !downPressed) {
        chordActive = false;
      }
      return {Event::None, true};
    }

    if (powerPressed && downPressed && chordAction != ChordAction::Disabled) {
      chordActive = true;
      if (quickLockState.isLocked() && chordAction != ChordAction::QuickLock) {
        return {Event::None, true};
      }
      switch (chordAction) {
        case ChordAction::Screenshot:
          return {Event::Screenshot, true};
        case ChordAction::QuickLock:
          (void)quickLockState.toggle(nowMs);
          return {Event::QuickLockChanged, true};
        case ChordAction::NextPage:
          return {Event::NextPage, true};
        case ChordAction::PreviousPage:
          return {Event::PreviousPage, true};
        case ChordAction::Disabled:
          break;
      }
    }

    if (powerReleased && quickLockOnShortPower) {
      (void)quickLockState.toggle(nowMs);
      return {Event::QuickLockChanged, true};
    }

    return {Event::None, quickLockState.isLocked()};
  }

  [[nodiscard]] bool isQuickLocked() const { return quickLockState.isLocked(); }
  void restoreQuickLock(uint32_t nowMs) {
    if (!quickLockState.isLocked()) {
      (void)quickLockState.toggle(nowMs);
    }
  }
  [[nodiscard]] bool shouldQuickLockSleep(uint32_t nowMs, uint32_t timeoutMs) const {
    return quickLockState.shouldSleep(nowMs, timeoutMs);
  }
  [[nodiscard]] bool isChordActive() const { return chordActive; }

 private:
  QuickLockState quickLockState;
  bool chordActive = false;
};
