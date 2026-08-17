#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable Rtc::DateTime _cachedDateTime{};
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;
  mutable const char* _activeTimeZoneRule = nullptr;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

  bool getUtcDateTime(Rtc::DateTime& dateTime) const;
  bool applyTimeZone(const char* posixTimeZone) const;

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // posixTimeZone: stable flash-resident POSIX TZ rule, including DST transitions when applicable.
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, const char* posixTimeZone = "UTC0", bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
};
