#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  Rtc::DateTime dt;
  if (!getUtcDateTime(dt)) return false;
  hour = dt.hour;
  minute = dt.minute;
  return true;
}

bool HalClock::getUtcDateTime(Rtc::DateTime& dateTime) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    dateTime = _cachedDateTime;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    dateTime = _cachedDateTime;
    return true;
  }
  _cachedDateTime = dt;
  _lastPollMs = now;
  _hasCachedTime = true;
  dateTime = _cachedDateTime;
  return true;
}

bool HalClock::applyTimeZone(const char* posixTimeZone) const {
  if (!posixTimeZone || posixTimeZone[0] == '\0') posixTimeZone = "UTC0";
  if (_activeTimeZoneRule == posixTimeZone) return true;
  if (setenv("TZ", posixTimeZone, 1) != 0) {
    LOG_ERR("CLK", "Failed to apply time zone");
    return false;
  }
  tzset();
  _activeTimeZoneRule = posixTimeZone;
  return true;
}

namespace {
// Gregorian calendar date to Unix-epoch day, without consulting the process TZ.
// This keeps the RTC value UTC while localtime_r applies the selected POSIX rule.
constexpr int64_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned shiftedMonth = month > 2 ? month - 3 : month + 9;
  const unsigned dayOfYear = (153 * shiftedMonth + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}
}  // namespace

bool HalClock::formatTime(char* buf, size_t bufSize, const char* posixTimeZone, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  Rtc::DateTime utc;
  if (!getUtcDateTime(utc) || !applyTimeZone(posixTimeZone)) return false;

  const int64_t seconds = daysFromCivil(utc.year, utc.month, utc.day) * 86400 + static_cast<int64_t>(utc.hour) * 3600 +
                          static_cast<int64_t>(utc.minute) * 60 + utc.second;
  const time_t epoch = static_cast<time_t>(seconds);
  struct tm localTime;
  if (!localtime_r(&epoch, &localTime)) return false;

  const int hour24 = localTime.tm_hour;
  const int min = localTime.tm_min;
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

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  _activeTimeZoneRule = nullptr;

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

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
        _cachedDateTime = dt;
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
