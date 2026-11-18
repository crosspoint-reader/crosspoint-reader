#pragma once
#include <cstdint>

// Minimal stand-in: only the fields Timezones.cpp reads.
struct CrossPointSettings {
  enum { CLOCK_DST_AUTO = 0, CLOCK_DST_ON = 1, CLOCK_DST_OFF = 2 };
  uint8_t clockTimezone = 255;
  uint8_t clockUtcOffsetQ = 255;
  uint8_t clockDst = CLOCK_DST_AUTO;
};
extern CrossPointSettings SETTINGS;
