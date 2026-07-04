#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the DS3231 RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59) from the DS3231 RTC (X3 only).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Get current UTC hour, minute, and second (RTC on X3, ESP32 system time on X4).
  bool getUtcTime(uint8_t& hour, uint8_t& minute, uint8_t& second) const;

  // True when UTC time can be read (RTC present or system clock set after NTP).
  bool hasValidUtcTime() const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false when UTC time is unavailable.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync time from NTP. On X3 writes the DS3231 RTC; on X4 sets ESP32 system time.
  // Requires WiFi to be connected. Blocks for up to ~10s while waiting for SNTP response.
  // Returns true if time was successfully updated and reads back as valid UTC.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // Primary NTP server hostname used by syncFromNTP().
  static const char* primaryNtpServer();

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
};
