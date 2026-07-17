#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  bool _softwareClockSynced = false;
  uint32_t _anchorEpoch = 0;
  int32_t _driftPpm = 0;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  bool isAvailable() const { return _available || _softwareClockSynced; }
  bool isHardwareAvailable() const { return _available; }

  void setCalibration(uint32_t anchorEpoch, int32_t driftPpm) { _anchorEpoch = anchorEpoch; _driftPpm = driftPpm; }
  uint32_t getAnchorEpoch() const { return _anchorEpoch; }
  int32_t getDriftPpm() const { return _driftPpm; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if no clock source is available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if no clock source is available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the clock from an NTP server. Requires WiFi to be connected.
  // On X3 writes the result to the DS3231. On X4 the SNTP stack updates time() directly.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true on success. Debouncing is enforced by the caller.
  bool syncFromNTP();

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
};
