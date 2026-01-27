#pragma once
#include <Arduino.h>
#include <BatteryMonitor.h>

#define BAT_GPIO0 0  // Battery voltage

static BatteryMonitor battery(BAT_GPIO0);

// Returns true when USB is connected (device is charging)
// Uses GPIO20 (U0RXD) which reads HIGH when USB is connected
inline bool isCharging() { return digitalRead(20) == HIGH; }
