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

    // While locked, Power + Down is the only way out, whatever the chord is set to.
    if (powerPressed && downPressed && (quickLockState.isLocked() || chordAction != ChordAction::Disabled)) {
      chordActive = true;
      if (quickLockState.isLocked()) {
        quickLockState.unlock();
        return {Event::QuickLockChanged, true};
      }
      switch (chordAction) {
        case ChordAction::Screenshot:
          return {Event::Screenshot, true};
        case ChordAction::QuickLock:
          quickLockState.lock(nowMs);
          return {Event::QuickLockChanged, true};
        case ChordAction::NextPage:
          return {Event::NextPage, true};
        case ChordAction::PreviousPage:
          return {Event::PreviousPage, true};
        case ChordAction::Disabled:
          break;
      }
    }

    if (powerReleased && quickLockOnShortPower && !quickLockState.isLocked()) {
      quickLockState.lock(nowMs);
      return {Event::QuickLockChanged, true};
    }

    return {Event::None, quickLockState.isLocked()};
  }

  [[nodiscard]] bool isQuickLocked() const { return quickLockState.isLocked(); }
  void lock(uint32_t nowMs) { quickLockState.lock(nowMs); }
  void noteActivity(uint32_t nowMs) { quickLockState.noteActivity(nowMs); }
  [[nodiscard]] bool shouldQuickLockSleep(uint32_t nowMs, uint32_t timeoutMs) const {
    return quickLockState.shouldSleep(nowMs, timeoutMs);
  }
  [[nodiscard]] bool isChordActive() const { return chordActive; }

 private:
  QuickLockState quickLockState;
  bool chordActive = false;
};

extern ButtonShortcutController buttonShortcutController;  // defined in main.cpp
