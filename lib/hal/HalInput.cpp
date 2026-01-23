#include "HalInput.h"
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
  // TODO
#endif
}

bool HalInput::isPressed(uint8_t buttonIndex) const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.isPressed(buttonIndex);
#else
  // TODO
  return false;
#endif
}

bool HalInput::wasPressed(uint8_t buttonIndex) const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasPressed(buttonIndex);
#else
  // TODO
  return false;
#endif
}

bool HalInput::wasAnyPressed() const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasAnyPressed();
#else
  // TODO
  return false;
#endif
}

bool HalInput::wasReleased(uint8_t buttonIndex) const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasReleased(buttonIndex);
#else
  // TODO
  return false;
#endif
}

bool HalInput::wasAnyReleased() const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.wasAnyReleased();
#else
  // TODO
  return false;
#endif
}

unsigned long HalInput::getHeldTime() const {
#if CROSSPOINT_EMULATED == 0
  return inputMgr.getHeldTime();
#else
  // TODO
  return 0;
#endif
}

void HalInput::setupGpioWakeup() {
#if CROSSPOINT_EMULATED == 0
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (isPressed(InputManager::BTN_POWER)) {
    delay(50);
    update();
  }
  // Enter Deep Sleep
  esp_deep_sleep_start();
#else
  // TODO
#endif
}
