#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

class HalClock;
extern HalClock halClock;

class HalClock {
  bool _available = false;

 public:
  void begin();
  bool isAvailable() const { return _available; }
  bool getTime(uint8_t& hour, uint8_t& minute) const;
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool formatTime(char* buf, size_t bufSize,
                  uint8_t utcOffsetQuarterHoursBiased = 48,
                  bool use12Hour = false) const;
  bool formatDate(char* buf, size_t bufSize,
                  uint8_t utcOffsetQuarterHoursBiased = 48) const;
  bool syncFromNTP();
};
