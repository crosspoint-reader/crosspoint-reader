#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

namespace {
constexpr int64_t MIN_SYSTEM_EPOCH = 1577836800;  // 2020-01-01 UTC
constexpr int64_t MAX_SYSTEM_EPOCH = 4102444800;  // 2100-01-01 UTC

bool validEpoch(const time_t epoch) { return epoch >= MIN_SYSTEM_EPOCH && epoch < MAX_SYSTEM_EPOCH; }

bool leapYear(const unsigned year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

// RTC fields are UTC. Validate before conversion rather than allowing mktime()
// to normalize corrupt dates or apply the display timezone.
bool rtcEpoch(const Rtc::DateTime& dt, time_t& epoch) {
  constexpr uint8_t MONTH_DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (dt.year < 2020 || dt.year >= 2100 || dt.month < 1 || dt.month > 12 || dt.hour > 23 || dt.minute > 59 ||
      dt.second > 59)
    return false;
  const unsigned daysInMonth = MONTH_DAYS[dt.month - 1] + (dt.month == 2 && leapYear(dt.year) ? 1 : 0);
  if (dt.day < 1 || dt.day > daysInMonth) return false;
  int64_t days = dt.day - 1;
  for (unsigned year = 1970; year < dt.year; ++year) days += leapYear(year) ? 366 : 365;
  for (unsigned month = 1; month < dt.month; ++month) {
    days += MONTH_DAYS[month - 1] + (month == 2 && leapYear(dt.year) ? 1 : 0);
  }
  epoch = static_cast<time_t>(days * 86400 + dt.hour * 3600 + dt.minute * 60 + dt.second);
  return validEpoch(epoch);
}
}  // namespace

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  if (_available && !hasValidSystemTime()) {
    Rtc::DateTime dt;
    time_t epoch = 0;
    if (_sdkRtc.now(dt) && rtcEpoch(dt, epoch)) {
      const timeval utc{epoch, 0};
      if (settimeofday(&utc, nullptr) != 0) {
        LOG_ERR("CLK", "Could not restore system UTC from RTC");
      }
    }
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

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

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

bool HalClock::hasValidSystemTime() const { return validEpoch(time(nullptr)); }

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && hasValidSystemTime()) {
      if (!_available) return true;
      time_t now = time(nullptr);
      struct tm timeinfo;
      if (!gmtime_r(&now, &timeinfo)) {
        LOG_ERR("CLK", "Could not convert synchronized UTC");
        return false;
      }

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
