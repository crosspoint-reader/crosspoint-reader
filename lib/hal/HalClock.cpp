#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)
//   0x03: Day of week (1-7, unused here)
//   0x04: Date    (01-31)
//   0x05: Month   (bits 4-0 = 01-12, bit 7 = century, kept 0 => 20xx)
//   0x06: Year    (00-99, i.e. 2000-2099)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

// Reject implausible RTC content: a DS3231 whose date registers were never
// written reads as 2000-01-01, which must not seed the system clock.
static constexpr int RTC_MIN_YEAR = 2020;

// UTC tm -> Unix epoch without TZ dependence (mktime honours the TZ env var,
// which is not guaranteed to be UTC at boot). Days-from-civil algorithm.
static time_t utcTmToEpoch(const struct tm& t) {
  int y = t.tm_year + 1900;
  const int m = t.tm_mon + 1;  // 1-12
  y -= m <= 2;
  const int era = y / 400;
  const int yoe = y - era * 400;                                           // [0, 399]
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;  // [0, 365]
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                   // [0, 146096]
  const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;  // days since 1970-01-01
  return static_cast<time_t>(days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
}

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Battery-backed RTC: restore the system clock at boot so FAT timestamps
  // (file browser date sort) are correct without any network.
  seedSystemClockFromRTC();

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::readDateTimeFromRTC(struct tm& utc) const {
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)7);
  if (Wire.available() < 7) return false;

  const uint8_t rawSec = Wire.read();
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();
  Wire.read();  // day of week — unused
  const uint8_t rawDate = Wire.read();
  const uint8_t rawMonth = Wire.read();
  const uint8_t rawYear = Wire.read();

  utc.tm_sec = bcdToDec(rawSec & 0x7F);
  utc.tm_min = bcdToDec(rawMin & 0x7F);
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    const bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    utc.tm_hour = pm ? (h12 + 12) : h12;
  } else {
    utc.tm_hour = bcdToDec(rawHour & 0x3F);
  }
  utc.tm_mday = bcdToDec(rawDate & 0x3F);
  utc.tm_mon = bcdToDec(rawMonth & 0x1F) - 1;
  utc.tm_year = 2000 + bcdToDec(rawYear) - 1900;  // century bit kept 0 => 20xx
  utc.tm_isdst = 0;
  return true;
}

void HalClock::seedSystemClockFromRTC() {
  struct tm utc{};
  if (!readDateTimeFromRTC(utc)) return;

  // Plausibility check: rejects factory-default / never-date-synced content
  const int year = utc.tm_year + 1900;
  if (year < RTC_MIN_YEAR || year > 2099 || utc.tm_mon < 0 || utc.tm_mon > 11 || utc.tm_mday < 1 || utc.tm_mday > 31 ||
      utc.tm_hour > 23 || utc.tm_min > 59 || utc.tm_sec > 59) {
    LOG_INF("CLK", "RTC date not set; system clock not seeded");
    return;
  }

  const struct timeval tv = {.tv_sec = utcTmToEpoch(utc), .tv_usec = 0};
  settimeofday(&tv, nullptr);
  LOG_INF("CLK", "System clock seeded from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC", year, utc.tm_mon + 1, utc.tm_mday,
          utc.tm_hour, utc.tm_min, utc.tm_sec);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
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

bool HalClock::writeDateTimeToRTC(const struct tm& utc) {
  const int year = utc.tm_year + 1900;
  assert(utc.tm_hour < 24);
  assert(utc.tm_min < 60);
  assert(utc.tm_sec < 60);
  if (year < 2000 || year > 2099) {
    LOG_ERR("CLK", "Year %d outside DS3231 range", year);
    return false;
  }

  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);                                  // Start at register 0x00
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_sec)));      // 0x00: Seconds
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_min)));      // 0x01: Minutes
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_hour)));     // 0x02: Hours (24h mode, bit 6 = 0)
  Wire.write(static_cast<uint8_t>(utc.tm_wday + 1));           // 0x03: Day of week (1-7)
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_mday)));     // 0x04: Date
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_mon + 1)));  // 0x05: Month (century bit 0)
  Wire.write(decToBcd(static_cast<uint8_t>(year - 2000)));     // 0x06: Year (00-99)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = static_cast<uint8_t>(utc.tm_hour);
  _cachedMinute = static_cast<uint8_t>(utc.tm_min);
  _hasCachedTime = true;
  return true;
}

bool HalClock::setFromSystemTime() {
  if (!_available) return false;

  const time_t now = time(nullptr);
  struct tm utc{};
  if (now < 1577836800 || gmtime_r(&now, &utc) == nullptr) {  // 2020-01-01: clock unset
    return false;
  }
  return writeDateTimeToRTC(utc);
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

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
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      // Full date+time so the RTC can re-seed the system clock at next boot
      if (writeDateTimeToRTC(timeinfo)) {
        LOG_INF("CLK", "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
