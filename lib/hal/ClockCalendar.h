#pragma once

#include <cstdint>
#include <ctime>

namespace ClockCalendar {

struct DateTime {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t weekday = 0;
};

bool isValid(const DateTime& dateTime);
bool toEpoch(const DateTime& dateTime, time_t& epoch);
bool fromEpoch(time_t epoch, DateTime& dateTime);
bool isValidSystemEpoch(time_t epoch);

}  // namespace ClockCalendar
