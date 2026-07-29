#pragma once

#include <cstdint>

class ButtonShortcutController {
 public:
  enum class ChordAction : uint8_t { Screenshot = 0, QuickLock = 1, NextPage = 2, PreviousPage = 3, Disabled = 4 };
  enum class Event : uint8_t { None, QuickLockChanged, Screenshot, NextPage, PreviousPage };

  struct Result {
    Event event = Event::None;
    bool consumeInput = false;
  };

  [[nodiscard]] Result update(bool powerPressed, bool downPressed, bool powerReleased, bool quickLockOnShortPower,
                              ChordAction chordAction) {
    if (chordActive) {
      if (!powerPressed && !downPressed) {
        chordActive = false;
      }
      return {Event::None, true};
    }

    if (powerPressed && downPressed && chordAction != ChordAction::Disabled) {
      chordActive = true;
      if (quickLocked && chordAction != ChordAction::QuickLock) {
        return {Event::None, true};
      }
      switch (chordAction) {
        case ChordAction::Screenshot:
          return {Event::Screenshot, true};
        case ChordAction::QuickLock:
          quickLocked = !quickLocked;
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
      quickLocked = !quickLocked;
      return {Event::QuickLockChanged, true};
    }

    return {Event::None, quickLocked};
  }

  [[nodiscard]] bool isQuickLocked() const { return quickLocked; }
  [[nodiscard]] bool isChordActive() const { return chordActive; }

 private:
  bool quickLocked = false;
  bool chordActive = false;
};
