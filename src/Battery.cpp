#include "Battery.h"

void BatteryPercentageRingBuffer::init(uint8_t v) {
  for (size_t i = 0; i < MAX_SAMPLES; i++) {
    buf[i] = v;
  }
  sum = v * MAX_SAMPLES;
  prev_val = v;
  head = 0;
  was_charging = true;
}

void BatteryPercentageRingBuffer::update(uint8_t v) {
  // Previous percentage is set > 100 only if buffer was constructed but not initialized yet
  if (prev_val > 100) {
    init(v);
    was_charging = false;
  }

  // Buffer was reintilialized with readout values while charging (can be too high values), reinit with new readout
  // after charging
  if (was_charging) {
    init(v);
    was_charging = false;
  }

  // Recalculate rolling sum
  sum -= buf[head];
  buf[head] = v;
  sum += v;

  // Shift head
  head++;
  if (head >= MAX_SAMPLES) head = 0;
}

uint8_t BatteryPercentageRingBuffer::evaluate() {
  uint8_t new_val = (sum + MAX_SAMPLES / 2) / MAX_SAMPLES;

  // Battery percentage should not increase when not charging so we just cap it to be lower than the last value
  if (new_val < prev_val) {
    prev_val = new_val;
    return new_val;
  } else {
    return prev_val;
  }
}
