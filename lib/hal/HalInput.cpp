#include "HalInput.h"
#include "EmulationUtils.h"
#include <esp_sleep.h>

void HalInput::begin() {
#if CROSSPOINT_EMULATED == 0
  inputMgr.begin();
#endif
}

void HalInput::update() {
#if CROSSPOINT_EMULATED == 0
  inputMgr.update();
#else
  const unsigned long currentTime = millis();

  EmulationUtils::sendCmd(EmulationUtils::CMD_BUTTON, "read");
  auto res = EmulationUtils::recvRespInt64();
  assert(res >= 0);

  const uint8_t state = static_cast<uint8_t>(res);

  // Always clear events first
  pressedEvents = 0;
  releasedEvents = 0;

  // Debounce
  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  static constexpr unsigned long DEBOUNCE_DELAY = 5;
  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      // Calculate pressed and released events
      pressedEvents = state & ~currentState;
      releasedEvents = currentState & ~state;

      // If pressing buttons and wasn't before, start recording time
      if (pressedEvents > 0 && currentState == 0) {
        buttonPressStart = currentTime;
      }

      // If releasing a button and no other buttons being pressed, record finish time
      if (releasedEvents > 0 && state == 0) {
        buttonPressFinish = currentTime;
      }

      currentState = state;
    }
  }
#endif
}

bool HalInput::isPressed(uint8_t buttonIndex) const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.isPressed(buttonIndex);
#else
  return currentState & (1 << buttonIndex);
#endif
}

bool HalInput::wasPressed(uint8_t buttonIndex) const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasPressed(buttonIndex);
#else
  return currentState & (1 << buttonIndex);
#endif
}

bool HalInput::wasAnyPressed() const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasAnyPressed();
#else
  return pressedEvents > 0;
#endif
}

bool HalInput::wasReleased(uint8_t buttonIndex) const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasReleased(buttonIndex);
#else
  return releasedEvents & (1 << buttonIndex);
#endif
}

bool HalInput::wasAnyReleased() const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasAnyReleased();
#else
  return releasedEvents > 0;
#endif
}

unsigned long HalInput::getHeldTime() const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.getHeldTime();
#else
  // Still hold a button
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
#endif
}

void startDeepSleep(InputManager& inputMgr) {
#if CROSSPOINT_EMULATED == 0
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(InputManager::BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Enter Deep Sleep
  esp_deep_sleep_start();
#else
  Serial.println("[   ] GPIO wakeup setup skipped in emulation mode.");
#endif
}
