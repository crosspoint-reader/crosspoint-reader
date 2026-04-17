#include "HalClock.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>  // Needed for I2C communication with the RTC
#include <esp_private/esp_clk.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cmath>
#include <cstdlib>

// ---- RTC / I2C configuration ----------------------------------------------
// Pins for ESP32-C3 (according to https://gist.github.com/CrazyCoder/1c5f846adee18e21f91e264601a6ddce)
static constexpr uint8_t DS3231_ADDRESS = 0x68;
static constexpr int I2C_SDA = 8;
static constexpr int I2C_SCL = 9;
static uint8_t bin2bcd(uint8_t val) { return val + 6 * (val / 10); }
static uint8_t bcd2bin(uint8_t val) { return val - 6 * (val >> 4); }

/**
 * Convert struct tm (interpreted as UTC) to Unix epoch seconds.
 * Replaces mktime(), as mktime considers the local timezone (TZ).
 */
static time_t timegm_compat(const struct tm* tm) {
  int32_t year = tm->tm_year + 1900;
  int32_t month = tm->tm_mon;  // 0-11

  // Helper calculation: days since the beginning of the year
  static const uint16_t days_before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

  // Days since 1970 (considering leap years)
  time_t days = (year - 1970) * 365 + (year - 1969) / 4;
  days += days_before_month[month];

  // Leap year correction for the current year (no extra day before March)
  if (month > 1 && (year % 4 == 0)) {
    days++;
  }
  days += tm->tm_mday - 1;

  return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

// ---- RTC-memory state (survives deep sleep, not cold boot) ----------------

static constexpr uint32_t CLOCK_RTC_MAGIC = 0xC10C4B1D;
static constexpr uint32_t CLOCK_RTC_FLAG_LP_VALID = 0x00000001u;

// Temperature drift model for ESP32 RTC-based timekeeping.
//
// The chip's low-power (slow) clock frequency depends on temperature.
// ESP32 variants can drift by about 2 minutes per day per °C from the
// initial captured operating temperature.
//
// - dt_drift ≈ 120 seconds/day/°C
// - relative frequency error per second per °C = 120 / 86400
//
// At restore() we apply a first-order correction over the sleep interval:
// corrected_interval = raw_interval × (1 + ΔT × drift_factor), where
// drift_factor = 120 / 86400.
//
// Experimental source: https://www.reddit.com/r/esp32/comments/11cikkp/the_clock_on_the_esp_is_wrong/
static constexpr float CLOCK_TEMP_DRIFT_SECONDS_PER_SECOND_PER_DEG = 120.0f / 86400.0f;

RTC_NOINIT_ATTR static uint32_t rtcClockMagic;
RTC_NOINIT_ATTR static uint32_t rtcClockFlags;
RTC_NOINIT_ATTR static time_t rtcEpoch;        // last-known unix epoch
RTC_NOINIT_ATTR static uint64_t rtcLpTimeUs;   // esp_clk_rtc_time() at capture
RTC_NOINIT_ATTR static uint32_t rtcSlowCal;    // esp_clk_slowclk_cal_get() at capture
RTC_NOINIT_ATTR static float rtcTemperatureC;  // captured chip temperature at save

static bool clockApproximate = true;

// Drift correction scale factor (learned from NTP sync results).
//
// Raw temp drift model uses 2 min/day/°C -> factor = 120/86400. This is a
// generic base model. The actual board may behave a bit differently. On each
// NTP sync we estimate how the local clock error compares to the model and
// update this scale factor slightly to converge toward real world behavior.
//
// rtcDriftScale = 1.0 means we trust 2 min/day/°C exactly. If the device is
// slower/faster than that, NTP drift calibration adjusts this factor.
static float rtcDriftScale = 1.0f;

static unsigned long lastPeriodicUpdateMs = 0;
static constexpr unsigned long PERIODIC_UPDATE_INTERVAL_MS = 10UL * 60UL * 1000UL;

struct TimeZoneEntry {
  const char* tz;
};

static constexpr TimeZoneEntry TIMEZONES[] = {
    {"GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"MSK-3"},
    {"UTC-4"},
    {"UTC-5:30"},
    {"UTC-7"},
    {"UTC-8"},
    {"UTC-9"},
    {"AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"NZST-12NZDT,M9.5.0/2,M4.1.0/3"},
    {"UTC+3"},
    {"EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"PST8PDT,M3.2.0/2,M11.1.0/2"},
};

// ---- NVS helpers ----------------------------------------------------------

// If the last NTP sync is older than this, treat a cold-boot restore as
// unsynced rather than showing a potentially very wrong time.
static constexpr int64_t STALE_THRESHOLD_S = 72 * 3600;  // 72 hours

static constexpr char NVS_NAMESPACE[] = "halclock";
static constexpr char NVS_KEY[] = "epoch";
static constexpr char NVS_SYNC_KEY[] = "lastsync";
static constexpr char NVS_DRIFT_KEY[] = "driftcoef";
static constexpr char NVS_TEMP_KEY[] = "lasttemp";

static void nvsWrite(time_t epoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_KEY, (int64_t)epoch);
    prefs.end();
  }
}

static void nvsWriteDriftScale(float driftScale) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putFloat(NVS_DRIFT_KEY, driftScale);
    prefs.end();
  }
}

static float nvsReadDriftScale() {
  Preferences prefs;
  float result = 1.0f;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    result = prefs.getFloat(NVS_DRIFT_KEY, 1.0f);
    prefs.end();
  }
  // Guard against NaN, Inf, or out-of-range values from corrupted NVS.
  if (!std::isfinite(result) || result < 0.1f || result > 5.0f) {
    result = 1.0f;
  }
  return result;
}

static void nvsWriteLastSyncTemp(float tempC) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putFloat(NVS_TEMP_KEY, tempC);
    prefs.end();
  }
}

static float nvsReadLastSyncTemp() {
  Preferences prefs;
  float result = 0.0f;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    result = prefs.getFloat(NVS_TEMP_KEY, 0.0f);
    prefs.end();
  }
  return result;
}

static void nvsWriteSyncTime(time_t syncEpoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_SYNC_KEY, (int64_t)syncEpoch);
    prefs.end();
  }
}

static time_t nvsRead() {
  Preferences prefs;
  time_t epoch = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    epoch = (time_t)prefs.getLong64(NVS_KEY, 0);
    prefs.end();
  }
  return epoch;
}

static time_t nvsReadSyncTime() {
  Preferences prefs;
  time_t syncEpoch = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    syncEpoch = (time_t)prefs.getLong64(NVS_SYNC_KEY, 0);
    prefs.end();
  }
  return syncEpoch;
}

// ---- internal helpers -----------------------------------------------------

static float readChipTemperatureC() {
  // ESP32 and ESP32-C3 use the internal ADC temperature sensor.
  if (initExternalRTC()) {
    return readExternalTemp();
  }
  return (float)temperatureRead();
}

// ---- New internal helpers for DS3231 ---------------------------------------

static bool initExternalRTC() {
  static bool initialized = false;
  static bool exists = false;
  if (initialized) return exists;
  initialized = true;

  if (!gpio.deviceIsX3()) {
    LOG_DBG("CLK", "Skipping DS3231 init on non-X3 board");
    return false;
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(DS3231_ADDRESS);
  if (Wire.endTransmission() == 0) {
    exists = true;
    LOG_INF("CLK", "DS3231 Hardware via I2C found.");
  } else {
    LOG_INF("CLK", "No DS3231 found.");
  }
  return exists;
}

// Write time to DS3231
static void writeExternalRTC(time_t t) {
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);  // DS3231 gets usually operated in UTC

  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(0x00);  // start-register (seconds)
  Wire.write(bin2bcd(timeinfo.tm_sec));
  Wire.write(bin2bcd(timeinfo.tm_min));
  Wire.write(bin2bcd(timeinfo.tm_hour));
  Wire.write(bin2bcd(0));  // weekday (ignored here)
  Wire.write(bin2bcd(timeinfo.tm_mday));
  Wire.write(bin2bcd(timeinfo.tm_mon + 1));
  Wire.write(bin2bcd(timeinfo.tm_year - 100));  // DS3231 stores years since 2000
  Wire.endTransmission();
}

// Read time from DS3231
static time_t readExternalRTC() {
  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return 0;

  Wire.requestFrom(DS3231_ADDRESS, (uint8_t)7);
  if (Wire.available() < 7) return 0;

  struct tm timeinfo = {};
  timeinfo.tm_sec = bcd2bin(Wire.read() & 0x7F);
  timeinfo.tm_min = bcd2bin(Wire.read());
  timeinfo.tm_hour = bcd2bin(Wire.read() & 0x3F);
  Wire.read();  // Wochentag überspringen
  timeinfo.tm_mday = bcd2bin(Wire.read());
  timeinfo.tm_mon = bcd2bin(Wire.read()) - 1;
  timeinfo.tm_year = bcd2bin(Wire.read()) + 100;
  timeinfo.tm_isdst = 0;

  return timegm_compat(&timeinfo);
}

// Read temperature (Register 0x11)
static float readExternalTemp() {
  Wire.beginTransmission(DS3231_ADDRESS);
  Wire.write(0x11);
  Wire.endTransmission();
  Wire.requestFrom(DS3231_ADDRESS, (uint8_t)2);

  int8_t msb = Wire.read();
  uint8_t lsb = Wire.read();
  return (float)msb + (lsb >> 6) * 0.25f;
}

// ----

static void setSystemClock(time_t epoch) {
  struct timeval tv = {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
}

static bool rtcValid() { return rtcClockMagic == CLOCK_RTC_MAGIC && rtcEpoch > 0; }

/// Compute temperature-corrected elapsed seconds from LP timer delta.
/// Uses the trapezoidal rule (average of start + end temperature) as a
/// first-order approximation of the temperature integral over the interval.
/// Returns the corrected elapsed seconds and updates lpNowOut/calNowOut
/// for the caller to re-baseline.
static double computeCorrectedElapsedSec(uint64_t lpNow, float tempNow) {
  uint32_t calNow = esp_clk_slowclk_cal_get();
  uint64_t elapsedUs;
  if (rtcSlowCal != 0 && calNow != 0) {
    // rtcLpTimeUs was computed with rtcSlowCal; convert it to the
    // current calibration basis so the subtraction is consistent.
    uint64_t lpThenCorrected = (uint64_t)((double)rtcLpTimeUs * calNow / rtcSlowCal);
    elapsedUs = lpNow - lpThenCorrected;
  } else {
    elapsedUs = lpNow - rtcLpTimeUs;
  }

  // Use the full temperature delta between the average over the interval
  // and the calibration reference (which is the capture-time temperature).
  // avgTemp approximates the mean temperature during the interval.
  // The drift model says the RTC runs (1 + deltaT * driftRate) times
  // faster/slower than nominal, so the true elapsed wall-clock time
  // differs from the raw LP-derived time by that factor.
  float avgTemp = (rtcTemperatureC + tempNow) * 0.5f;
  // Positive when COOLED DOWN relative to capture temperature.
  // ESP32 RC oscillator has a positive temperature coefficient: it runs faster
  // when hotter, causing the LP timer to over-count. To recover true elapsed
  // time we must REDUCE the raw LP-derived seconds when the device is warmer
  // than at capture (and INCREASE them when cooler). Hence the sign inversion.
  float tempDelta = rtcTemperatureC - avgTemp;  // = (rtcTemperatureC - tempNow) / 2
  float tempFactor = 1.0f + tempDelta * CLOCK_TEMP_DRIFT_SECONDS_PER_SECOND_PER_DEG * rtcDriftScale;
  if (tempFactor < 0.5f) {
    tempFactor = 0.5f;
  } else if (tempFactor > 1.5f) {
    tempFactor = 1.5f;
  }

  double elapsedSec = (double)elapsedUs / 1000000.0;
  double correctedSec = elapsedSec * (double)tempFactor;

  LOG_DBG("CLK", "Drift calc: startT=%.1fC nowT=%.1fC dT=%.3f factor=%.6f raw=%.3fs corr=%.3fs", rtcTemperatureC,
          tempNow, tempDelta, tempFactor, elapsedSec, correctedSec);

  return correctedSec;
}

/// Capture current time + LP timer into RTC memory, and epoch into NVS.
static void capture(bool lpValid) {
  rtcEpoch = time(nullptr);
  // Update DS3231 only when the current time is authoritative.
  if (initExternalRTC() && !clockApproximate) {
    writeExternalRTC(rtcEpoch);
  }
  rtcLpTimeUs = esp_clk_rtc_time();
  rtcSlowCal = esp_clk_slowclk_cal_get();
  rtcTemperatureC = readChipTemperatureC();
  rtcClockMagic = CLOCK_RTC_MAGIC;
  rtcClockFlags = lpValid ? CLOCK_RTC_FLAG_LP_VALID : 0;
  nvsWrite(rtcEpoch);
}

// ---- public API -----------------------------------------------------------

namespace HalClock {

void applyTimezone(uint8_t timeZoneSetting) {
  const size_t index = timeZoneSetting < (sizeof(TIMEZONES) / sizeof(TIMEZONES[0])) ? timeZoneSetting : 0;
  setenv("TZ", TIMEZONES[index].tz, 1);
  tzset();
  LOG_DBG("CLK", "Timezone applied: %s", TIMEZONES[index].tz);
}

bool syncNtp() {
  time_t preSyncTime = time(nullptr);
  time_t prevSyncTime = nvsReadSyncTime();
  float prevSyncTemp = nvsReadLastSyncTemp();

  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  constexpr int maxRetries = 50;  // 5 seconds
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry >= maxRetries) {
    LOG_ERR("CLK", "NTP sync timeout");
    return false;
  }

  // NTP sync yields authoritative time; allow DS3231 to be updated.
  clockApproximate = false;
  capture(false);
  nvsWriteSyncTime(rtcEpoch);

  float currentTemp = rtcTemperatureC;
  if (currentTemp != 0.0f) {
    nvsWriteLastSyncTemp(currentTemp);
  }

  if (prevSyncTime > 0 && preSyncTime > 0 && rtcEpoch > prevSyncTime) {
    float interval = (float)(rtcEpoch - prevSyncTime);
    // error = how far the local clock was off before NTP corrected it.
    // Negative means local clock was behind (NTP jumped us forward).
    // Positive means local clock was ahead (NTP pulled us back).
    float error = (float)(preSyncTime - rtcEpoch);
    if (interval >= 60.0f) {
      // Convert to seconds-of-drift per day.
      float observedDriftPerDay = error * 86400.0f / interval;

      // Adaptive model calibration:
      // - Observed drift is derived from the difference between local clock
      //   reading just before NTP and the true time reported by NTP, scaled
      //   to a per-day rate over the interval since the previous sync.
      // - The baseline model expects 120 sec/day per °C.
      // - Measure temp delta since last sync (from stored NVS temp).
      // - If large enough, compute an empirical scale to apply to the model
      //   so future drift corrections are better aligned with actual hardware.
      // - The scale is persisted to NVS via saveBeforeSleep().
      float effectiveScale = rtcDriftScale;
      float tempDelta = currentTemp - prevSyncTemp;
      if (std::fabs(tempDelta) > 0.1f) {
        float modelDriftPerDay = 120.0f * tempDelta;
        if (std::fabs(modelDriftPerDay) > 0.01f) {
          float measuredScale = observedDriftPerDay / modelDriftPerDay;
          effectiveScale = 0.9f * rtcDriftScale + 0.1f * measuredScale;
          effectiveScale = std::max(0.1f, std::min(5.0f, effectiveScale));
          rtcDriftScale = effectiveScale;
        }
      }

      LOG_DBG("CLK", "NTP drift: interval=%.0fs error=%.3fs perDay=%.3f scale=%.3f deltaT=%.2f", interval, error,
              observedDriftPerDay, rtcDriftScale, tempDelta);
    }
  }

  clockApproximate = false;
  LOG_INF("CLK", "NTP synced, epoch %lld", (long long)rtcEpoch);
  return true;
}

void saveBeforeSleep(bool keepLpAlive) {
  if (!isSynced()) {
    return;
  }
  capture(keepLpAlive);
  // Persist learned drift scale and last temperature to NVS so they survive
  // cold boot. We only write here (not periodically) to minimise flash wear.
  nvsWriteDriftScale(rtcDriftScale);
  nvsWriteLastSyncTemp(rtcTemperatureC);
  LOG_DBG("CLK", "Saved epoch %lld before sleep (driftScale=%.3f)", (long long)rtcEpoch, rtcDriftScale);
}

void restore() {
  // PRIORITY 1: DS3231 (Hardware-RTC)
  if (initExternalRTC()) {
    time_t rtcTime = readExternalRTC();
    if (rtcTime > 1577836800) {  // Check if time is after 2020 (plausible timestamp)
      setSystemClock(rtcTime);
      rtcEpoch = rtcTime;
      clockApproximate = false;
      LOG_INF("CLK", "Got time from DS3231.");
      return;
    }
  }

  rtcDriftScale = nvsReadDriftScale();
  const bool lpValid = (rtcClockFlags & CLOCK_RTC_FLAG_LP_VALID) != 0;
  if (rtcValid() && lpValid) {
    // RTC memory survived — we woke from deep sleep.
    //
    // We restore the wall clock by computing elapsed real time from the
    // LP timer delta and applying both frequency calibration and temperature
    // drift correction.
    //
    // Steps:
    // 1) Read current LP timer and slow-clock calibration.
    // 2) Compute raw elapsed LP ticks, on the same calibration basis used
    //    when capture() was called.
    // 3) Convert elapsed ticks to seconds.
    // 4) Apply temperature drift correction based on measured RTC memory
    //    capture temperature and current chip temp.
    // 5) Set system time to rtcEpoch + corrected elapsed seconds.
    //
    // This is an approximation: we use the average of start/end measured
    // temperature as a simple integral proxy. More advanced models could
    // sample temperature continuously, but this is a good tradeoff for low
    // cost and better accuracy vs no temperature compensation.
    uint64_t lpNow = esp_clk_rtc_time();
    time_t estimated = rtcEpoch;
    if (lpNow > rtcLpTimeUs) {
      float tempNow = readChipTemperatureC();
      double correctedSec = computeCorrectedElapsedSec(lpNow, tempNow);
      estimated += (time_t)correctedSec;
    }

    setSystemClock(estimated);
    // Re-baseline LP timer and temperature for next interval.
    rtcEpoch = estimated;
    rtcLpTimeUs = esp_clk_rtc_time();
    rtcSlowCal = esp_clk_slowclk_cal_get();
    rtcTemperatureC = readChipTemperatureC();
    clockApproximate = true;
    LOG_INF("CLK", "Restored from RTC + LP timer, epoch %lld", (long long)estimated);
    return;
  }

  // Cold boot — try NVS.  No elapsed correction possible.
  time_t epoch = nvsRead();
  if (epoch > 0) {
    time_t lastSync = nvsReadSyncTime();
    if (lastSync > 0 && (epoch - lastSync) > STALE_THRESHOLD_S) {
      LOG_ERR("CLK", "NVS epoch %lld is stale (last NTP sync %lld, %lld h ago), discarding", (long long)epoch,
              (long long)lastSync, (long long)((epoch - lastSync) / 3600));
      return;
    }
    setSystemClock(epoch);
    rtcEpoch = epoch;
    rtcLpTimeUs = esp_clk_rtc_time();
    rtcSlowCal = esp_clk_slowclk_cal_get();
    rtcTemperatureC = nvsReadLastSyncTemp();
    if (rtcTemperatureC == 0.0f) {
      rtcTemperatureC = readChipTemperatureC();
    }
    rtcClockMagic = CLOCK_RTC_MAGIC;
    rtcClockFlags = 0;
    clockApproximate = true;
    LOG_INF("CLK", "Restored from NVS, epoch %lld (no elapsed correction)", (long long)epoch);
  }
}

time_t now() {
  if (!isSynced()) {
    return 0;
  }
  return time(nullptr);
}

void updatePeriodic() {
  // DS3231 (if present) has priority, synchronize the system time
  // every 10 minutes directly against the RTC, instead of calculating.
  if (initExternalRTC()) {
    time_t rtcTime = readExternalRTC();
    unsigned long nowMs = millis();
    if ((nowMs - lastPeriodicUpdateMs >= PERIODIC_UPDATE_INTERVAL_MS) &&
        (rtcTime > 1577836800)) {  // Check if time is after 2020 (plausible timestamp)
      lastPeriodicUpdateMs = nowMs;
      setSystemClock(rtcTime);
      LOG_DBG("CLK", "Systemtime has been taken from DS3231");
    }
    return;
  }

  if (!isSynced()) {
    return;
  }
  unsigned long nowMs = millis();
  if (nowMs - lastPeriodicUpdateMs < PERIODIC_UPDATE_INTERVAL_MS) {
    return;
  }
  lastPeriodicUpdateMs = nowMs;

  // Compute temperature-corrected elapsed time since last baseline and apply
  // only the drift delta (correction - raw) to the system clock. The kernel
  // clock already advanced by the raw amount, so we must not re-add it.
  uint64_t lpNow = esp_clk_rtc_time();
  if (lpNow <= rtcLpTimeUs) {
    return;
  }

  float tempNow = readChipTemperatureC();
  double correctedSec = computeCorrectedElapsedSec(lpNow, tempNow);

  // Raw elapsed seconds (what the kernel clock already counted).
  uint64_t rawElapsedUs = lpNow - rtcLpTimeUs;
  double rawSec = (double)rawElapsedUs / 1000000.0;

  // The drift delta is the difference between what really elapsed
  // (temperature-corrected) and what the kernel counted (raw).
  double driftDeltaSec = correctedSec - rawSec;

  // Re-baseline LP timer and temperature for the next interval.
  rtcLpTimeUs = lpNow;
  rtcSlowCal = esp_clk_slowclk_cal_get();
  rtcTemperatureC = tempNow;

  // Only nudge the system clock if the drift delta is meaningful (>50 ms).
  // This avoids unnecessary settimeofday calls for negligible corrections.
  if (std::fabs(driftDeltaSec) > 0.05) {
    rtcEpoch = time(nullptr) + (time_t)driftDeltaSec;
    setSystemClock(rtcEpoch);
    LOG_DBG("CLK", "Periodic drift nudge: raw=%.3fs corr=%.3fs delta=%.3fs scale=%.3f", rawSec, correctedSec,
            driftDeltaSec, rtcDriftScale);
  }
}

bool isSynced() {
  return time(nullptr) > 1577836800;  // > 2020-01-01
}

bool isApproximate() { return clockApproximate; }

time_t lastSyncTime() { return nvsReadSyncTime(); }

void formatTime(char* buf, size_t bufSize, bool use24h) {
  if (!isSynced()) {
    snprintf(buf, bufSize, "--:--");
    return;
  }

  time_t t = time(nullptr);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);

  const char* prefix = isApproximate() ? "~" : "";

  if (use24h) {
    snprintf(buf, bufSize, "%s%02d:%02d", prefix, timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    int hour = timeinfo.tm_hour % 12;
    if (hour == 0) hour = 12;
    const char* ampm = timeinfo.tm_hour < 12 ? "AM" : "PM";
    snprintf(buf, bufSize, "%s%d:%02d%s", prefix, hour, timeinfo.tm_min, ampm);
  }
}

void formatLogTime(char* buf, size_t bufSize) {
  if (!isSynced()) {
    buf[0] = '\0';
    return;
  }

  time_t t = time(nullptr);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  snprintf(buf, bufSize, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void wifiOff(bool skipNtpSync) {
  if (!skipNtpSync && isApproximate() && WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
    syncNtp();
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

}  // namespace HalClock
