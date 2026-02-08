#pragma once
#include <BatteryMonitor.h>

#include <cstddef>

#define BAT_GPIO0 0  // Battery voltage

static BatteryMonitor battery(BAT_GPIO0);

struct BatteryPercentageRingBuffer {
  static constexpr uint8_t MAX_SAMPLES = 10;

  uint8_t buf[MAX_SAMPLES];
  uint8_t head = 0;
  uint16_t sum = 0;
  uint8_t prev_val = 161;
  bool was_charging = false;

  void init(uint8_t value);
  void update(uint8_t value);
  uint8_t evaluate();
};
