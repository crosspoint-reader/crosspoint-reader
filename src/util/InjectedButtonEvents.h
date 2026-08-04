#pragma once

#include <cstdint>

class InjectedButtonEvents {
 public:
  void injectClick(const uint8_t buttonIndex) {
    const uint16_t mask = static_cast<uint16_t>(1U << buttonIndex);
    pressedMask |= mask;
    releasedMask |= mask;
  }

  [[nodiscard]] bool wasPressed(const uint8_t buttonIndex) const {
    return (pressedMask & static_cast<uint16_t>(1U << buttonIndex)) != 0;
  }

  [[nodiscard]] bool wasReleased(const uint8_t buttonIndex) const {
    return (releasedMask & static_cast<uint16_t>(1U << buttonIndex)) != 0;
  }

  void clear() {
    pressedMask = 0;
    releasedMask = 0;
  }

 private:
  uint16_t pressedMask = 0;
  uint16_t releasedMask = 0;
};
