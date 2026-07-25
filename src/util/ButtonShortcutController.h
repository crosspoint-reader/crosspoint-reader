#pragma once

#include <cstdint>

class ButtonShortcutController {
 public:
  enum class Event : uint8_t { None, QuickLockChanged };

  struct Result {
    Event event = Event::None;
    bool consumeInput = false;
  };

  [[nodiscard]] Result update(bool powerReleased, bool quickLockOnShortPower) {
    if (powerReleased && quickLockOnShortPower) {
      quickLocked = !quickLocked;
      return {Event::QuickLockChanged, true};
    }

    return {Event::None, quickLocked};
  }

  [[nodiscard]] bool isQuickLocked() const { return quickLocked; }

 private:
  bool quickLocked = false;
};
