#pragma once

#include <cstdint>

class Rtc {
 public:
  struct DateTime {
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 0;
  };

  bool begin();
  bool now(DateTime& output);
  bool set(const DateTime& value);
};
