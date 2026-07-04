#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

// Epoch threshold: Jan 2001 — rejects unset ESP32 system time after cold boot.
static constexpr time_t kMinValidUnixTime = 978307200;

static bool readRtcUtcHms(uint8_t& hour, uint8_t& minute, uint8_t& second) {
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    return false;
  }

  const uint8_t rawSec = Wire.read();
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  second = bcdToDec(rawSec & 0x7F);
  minute = bcdToDec(rawMin & 0x7F);
  if (rawHour & 0x40) {
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    const bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    hour = pm ? static_cast<uint8_t>(h12 + 12) : h12;
  } else {
    hour = bcdToDec(rawHour & 0x3F);
  }
  return true;
}

static bool readSystemUtcHms(uint8_t& hour, uint8_t& minute, uint8_t& second) {
  const time_t now = time(nullptr);
  if (now < kMinValidUnixTime) {
    return false;
  }
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  hour = static_cast<uint8_t>(timeinfo.tm_hour);
  minute = static_cast<uint8_t>(timeinfo.tm_min);
  second = static_cast<uint8_t>(timeinfo.tm_sec);
  return true;
}

static constexpr const char* kPrimaryNtpServer = "pool.ntp.org";
static constexpr const char* kSecondaryNtpServer = "time.nist.gov";

const char* HalClock::primaryNtpServer() { return kPrimaryNtpServer; }

static bool waitForSntpSync() {
  // Stop and re-init SNTP so manual retries work (configTzTime alone won't restart).
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  setenv("TZ", "UTC0", 1);
  tzset();

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, kPrimaryNtpServer);
  esp_sntp_setservername(1, kSecondaryNtpServer);
  esp_sntp_init();

  constexpr int maxAttempts = 100;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      const time_t now = time(nullptr);
      if (now >= kMinValidUnixTime) {
        LOG_INF("CLK", "NTP synced via %s (unix %ld)", kPrimaryNtpServer, static_cast<long>(now));
        return true;
      }
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out (server %s)", kPrimaryNtpServer);
  return false;
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

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getUtcTime(uint8_t& hour, uint8_t& minute, uint8_t& second) const {
  if (_available) {
    return readRtcUtcHms(hour, minute, second);
  }
  return readSystemUtcHms(hour, minute, second);
}

bool HalClock::hasValidUtcTime() const {
  uint8_t h = 0;
  uint8_t m = 0;
  uint8_t s = 0;
  return getUtcTime(h, m, s);
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
  uint8_t second = 0;
  if (!readRtcUtcHms(hour, minute, second)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  _cachedMinute = minute;
  _cachedHour = hour;
  _lastPollMs = now;
  _hasCachedTime = true;

  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h = 0;
  uint8_t m = 0;
  uint8_t s = 0;
  if (!getUtcTime(h, m, s)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
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

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync via %s...", kPrimaryNtpServer);
  if (!waitForSntpSync()) {
    return false;
  }

  if (_available) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    if (writeTimeToRTC(static_cast<uint8_t>(timeinfo.tm_hour), static_cast<uint8_t>(timeinfo.tm_min),
                       static_cast<uint8_t>(timeinfo.tm_sec))) {
      LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      return hasValidUtcTime();
    }
    return false;
  }

  if (!hasValidUtcTime()) {
    LOG_ERR("CLK", "NTP reported success but UTC time is still invalid");
    return false;
  }

  LOG_INF("CLK", "System time synced from NTP (no hardware RTC)");
  return true;
}
