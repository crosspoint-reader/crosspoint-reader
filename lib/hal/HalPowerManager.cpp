#include <esp_sleep.h>

#include "HalPowerManager.h"
#include "HalGPIO.h"

void HalPowerManager::begin() {
  pinMode(BAT_GPIO0, INPUT);
  normalFreq = getCpuFrequencyMhz();
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return; // invalid state
  }
  if (enabled && !isLowPower) {
    Serial.printf("[%lu] [PWR] Going to low-power mode\n", millis());
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      Serial.printf("[%lu] [PWR] Failed to set low-power CPU frequency\n", millis());
      return;
    }
  }
  if (!enabled && isLowPower) {
    Serial.printf("[%lu] [PWR] Restoring normal CPU frequency\n", millis());
    if (!setCpuFrequencyMhz(normalFreq)) {
      Serial.printf("[%lu] [PWR] Failed to restore normal CPU frequency\n", millis());
      return;
    }
  }
  isLowPower = enabled;
}

void HalPowerManager::startDeepSleep(HalGPIO &gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

int HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  return battery.readPercentage();
}
