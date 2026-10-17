#pragma once

// Browser profile: a fixed full battery, with no fuel-gauge persistence.
class BatteryMonitor {
 public:
  BatteryMonitor(int, int = -1) {}
  void begin() {}
  int getVoltage() { return 4200; }
  int getPercentage() { return 100; }
  static bool loadDesignCapacity() { return false; }
};
