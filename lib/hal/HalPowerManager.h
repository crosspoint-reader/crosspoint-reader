#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>

#include "HalGPIO.h"

class HalPowerManager {
  static constexpr int LOW_POWER_FREQ = 10; // MHz

  int normalFreq = 0; // MHz
  bool isLowPower = false;

 public:
  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep(HalGPIO &gpio) const;

  // Get battery percentage (range 0-100)
  int getBatteryPercentage() const;
};
