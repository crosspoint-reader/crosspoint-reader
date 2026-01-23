#pragma once
#include <BatteryMonitor.h>

#define BAT_GPIO0 0  // Battery voltage

class Battery {
 public:
  uint16_t readPercentage() const {
#if CROSSPOINT_EMULATED == 0
    static const BatteryMonitor hwBattery = BatteryMonitor(BAT_GPIO0);
    return hwBattery.readPercentage();
#else
    return 100;
#endif
  }
};

static Battery battery;
