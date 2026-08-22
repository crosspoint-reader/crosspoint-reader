#include "TerminalInput.h"

#include "MappedInputManager.h"
#include "TerminalWifi.h"

void TerminalInput::send(const char* sequence, const unsigned int length) const {
  wifi.write(reinterpret_cast<const uint8_t*>(sequence), length);
}

bool TerminalInput::poll() {
  if (input.wasPressed(MappedInputManager::Button::Up)) {
    send("\x1b[A", 3);
  }
  if (input.wasPressed(MappedInputManager::Button::Down)) {
    send("\x1b[B", 3);
  }
  if (input.wasPressed(MappedInputManager::Button::Right)) {
    send("\x1b[C", 3);
  }
  if (input.wasPressed(MappedInputManager::Button::Left)) {
    send("\x1b[D", 3);
  }

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (input.getHeldTime() >= LONG_PRESS_MS) {
      wifi.write(uint8_t{0x03});
    } else {
      wifi.write(static_cast<uint8_t>('\r'));
    }
  }

  if (input.wasReleased(MappedInputManager::Button::Back)) {
    if (input.getHeldTime() >= LONG_PRESS_MS) {
      return true;
    }
    wifi.write(uint8_t{0x1b});
  }
  return false;
}
