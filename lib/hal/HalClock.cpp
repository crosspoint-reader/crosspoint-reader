#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>

#include "ClockCalendar.h"

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  if (!_rtc.begin()) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  Rtc::DateTime rtcNow;
  if (!_rtc.now(rtcNow)) {
    LOG_INF("CLK", "DS3231 calendar is not valid yet");
    return;
  }
  ClockCalendar::DateTime calendar{rtcNow.year, rtcNow.month, rtcNow.day, rtcNow.hour,
                                   rtcNow.minute, rtcNow.second, rtcNow.weekday};
  time_t epoch = 0;
  if (!ClockCalendar::toEpoch(calendar, epoch)) {
    LOG_ERR("CLK", "DS3231 returned an invalid calendar");
    return;
  }
  const timeval systemTime{epoch, 0};
  if (settimeofday(&systemTime, nullptr) == 0) {
    _cachedHour = rtcNow.hour;
    _cachedMinute = rtcNow.minute;
    _hasCachedTime = true;
    _lastPollMs = millis();
    LOG_INF("CLK", "System clock restored from DS3231 calendar");
  }
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime rtcNow;
  if (!_rtc.now(rtcNow)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  const ClockCalendar::DateTime calendar{rtcNow.year, rtcNow.month, rtcNow.day, rtcNow.hour,
                                         rtcNow.minute, rtcNow.second, rtcNow.weekday};
  if (!ClockCalendar::isValid(calendar)) return false;

  _cachedMinute = rtcNow.minute;
  _cachedHour = rtcNow.hour;
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;
  Rtc::DateTime rtcNow;
  if (!_rtc.now(rtcNow)) return false;
  const ClockCalendar::DateTime calendar{rtcNow.year, rtcNow.month, rtcNow.day, rtcNow.hour,
                                         rtcNow.minute, rtcNow.second, rtcNow.weekday};
  if (!ClockCalendar::isValid(calendar)) return false;
  year = rtcNow.year;
  month = rtcNow.month;
  day = rtcNow.day;
  hour = rtcNow.hour;
  minute = rtcNow.minute;
  return true;
}

bool HalClock::isSystemTimeValid() const { return ClockCalendar::isValidSystemEpoch(time(nullptr)); }

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeDateTimeToRTC(const time_t epoch) {
  if (!_available) return true;
  ClockCalendar::DateTime calendar;
  if (!ClockCalendar::fromEpoch(epoch, calendar)) return false;
  const Rtc::DateTime rtcNow{calendar.year, calendar.month, calendar.day, calendar.hour,
                             calendar.minute, calendar.second, calendar.weekday};
  if (!_rtc.set(rtcNow)) return false;

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = calendar.hour;
  _cachedMinute = calendar.minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      if (!ClockCalendar::isValidSystemEpoch(now)) continue;
      if (writeDateTimeToRTC(now)) {
        LOG_INF("CLK", "System clock synchronized; external RTC updated when present");
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
