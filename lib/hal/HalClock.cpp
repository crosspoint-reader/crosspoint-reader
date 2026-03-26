#include "HalClock.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_private/esp_clk.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cstdlib>

// ---- RTC-memory state (survives deep sleep, not cold boot) ----------------

static constexpr uint32_t CLOCK_RTC_MAGIC = 0xC10C4B1D;
static constexpr uint32_t CLOCK_RTC_FLAG_LP_VALID = 0x00000001u;

RTC_NOINIT_ATTR static uint32_t rtcClockMagic;
RTC_NOINIT_ATTR static uint32_t rtcClockFlags;
RTC_NOINIT_ATTR static time_t rtcEpoch;       // last-known unix epoch
RTC_NOINIT_ATTR static uint64_t rtcLpTimeUs;  // esp_clk_rtc_time() at capture

static bool clockApproximate = true;

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

static constexpr char NVS_NAMESPACE[] = "halclock";
static constexpr char NVS_KEY[] = "epoch";

static void nvsWrite(time_t epoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_KEY, (int64_t)epoch);
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

// ---- internal helpers -----------------------------------------------------

static void setSystemClock(time_t epoch) {
  struct timeval tv = {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
}

static bool rtcValid() { return rtcClockMagic == CLOCK_RTC_MAGIC && rtcEpoch > 0; }

/// Capture current time + LP timer into RTC memory, and epoch into NVS.
static void capture(bool lpValid) {
  rtcEpoch = time(nullptr);
  rtcLpTimeUs = esp_clk_rtc_time();
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

  capture(false);
  clockApproximate = false;
  LOG_INF("CLK", "NTP synced, epoch %lld", (long long)rtcEpoch);
  return true;
}

void saveBeforeSleep(bool keepLpAlive) {
  if (!isSynced()) {
    return;
  }
  capture(keepLpAlive);
  LOG_DBG("CLK", "Saved epoch %lld before sleep", (long long)rtcEpoch);
}

void restore() {
  const bool lpValid = (rtcClockFlags & CLOCK_RTC_FLAG_LP_VALID) != 0;
  if (rtcValid() && lpValid) {
    // RTC memory survived — we woke from deep sleep.
    // Use the LP timer to compute how much time elapsed during sleep.
    uint64_t lpNow = esp_clk_rtc_time();
    time_t estimated = rtcEpoch;
    if (lpNow > rtcLpTimeUs) {
      estimated += (time_t)((lpNow - rtcLpTimeUs) / 1000000LL);
    }
    setSystemClock(estimated);
    // Re-capture with current LP baseline
    rtcEpoch = estimated;
    rtcLpTimeUs = lpNow;
    clockApproximate = true;
    LOG_INF("CLK", "Restored from RTC + LP timer, epoch %lld", (long long)estimated);
    return;
  }

  // Cold boot — try NVS.  No elapsed correction possible.
  time_t epoch = nvsRead();
  if (epoch > 0) {
    setSystemClock(epoch);
    rtcEpoch = epoch;
    rtcLpTimeUs = esp_clk_rtc_time();
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

bool isSynced() {
  return time(nullptr) > 1577836800;  // > 2020-01-01
}

bool isApproximate() { return clockApproximate; }

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
    const char* ampm = timeinfo.tm_hour < 12 ? "am" : "pm";
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

}  // namespace HalClock
