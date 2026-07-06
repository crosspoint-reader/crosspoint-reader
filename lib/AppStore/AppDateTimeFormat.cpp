#include "AppDateTimeFormat.h"

#include <cstdio>
#include <ctime>

namespace {

constexpr const char* kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool parseIso8601Utc(const std::string& isoUtc, int& year, int& month, int& day, int& hour, int& minute,
                     int& second) {
  if (isoUtc.size() < 16) {
    return false;
  }

  const int matched = sscanf(isoUtc.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  if (matched < 5) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  if (matched < 6) {
    second = 0;
  }
  return true;
}

int applyUtcOffsetMinutes(int hour, int minute, int second, int offsetQuarterHours, int& outHour, int& outMinute,
                          int& outSecond) {
  int totalMinutes = hour * 60 + minute + offsetQuarterHours * 15;
  const int dayMinutes = 24 * 60;
  totalMinutes = ((totalMinutes % dayMinutes) + dayMinutes) % dayMinutes;
  outHour = totalMinutes / 60;
  outMinute = totalMinutes % 60;
  outSecond = second;
  return 0;
}

}  // namespace

namespace AppDateTimeFormat {

std::string formatNowIso8601Utc() {
  const time_t now = time(nullptr);
  if (now < 978307200) {
    return "";
  }

  struct tm utc {};
#if defined(__unix__) || defined(__APPLE__)
  gmtime_r(&now, &utc);
#else
  const struct tm* tmp = gmtime(&now);
  if (tmp == nullptr) {
    return "";
  }
  utc = *tmp;
#endif

  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  return buf;
}

std::string formatNowIso8601UtcMinusSeconds(const uint32_t secondsAgo) {
  const time_t now = time(nullptr);
  if (now < 978307200) {
    return "";
  }

  const time_t adjusted = now - static_cast<time_t>(secondsAgo);
  const time_t clamped = adjusted < 978307200 ? 978307200 : adjusted;

  struct tm utc {};
#if defined(__unix__) || defined(__APPLE__)
  gmtime_r(&clamped, &utc);
#else
  const struct tm* tmp = gmtime(&clamped);
  if (tmp == nullptr) {
    return "";
  }
  utc = *tmp;
#endif

  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  return buf;
}

std::string formatIso8601ForDisplay(const std::string& isoUtc, const uint8_t utcOffsetQuarterHoursBiased,
                                    const bool use12Hour) {
  if (isoUtc.empty()) {
    return "";
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parseIso8601Utc(isoUtc, year, month, day, hour, minute, second)) {
    return "";
  }

  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  if (utcOffsetQuarterHoursBiased > 104) {
    offsetQuarterHours = 0;
  }

  int localHour = hour;
  int localMinute = minute;
  int localSecond = second;
  applyUtcOffsetMinutes(hour, minute, second, offsetQuarterHours, localHour, localMinute, localSecond);

  char buf[40];
  const char* monthName = (month >= 1 && month <= 12) ? kMonthNames[month - 1] : "???";
  if (use12Hour) {
    const bool pm = localHour >= 12;
    int hour12 = localHour % 12;
    if (hour12 == 0) {
      hour12 = 12;
    }
    snprintf(buf, sizeof(buf), "%s %d, %d %d:%02d %s", monthName, day, year, hour12, localMinute, pm ? "PM" : "AM");
  } else {
    snprintf(buf, sizeof(buf), "%s %d, %d %02d:%02d", monthName, day, year, localHour, localMinute);
  }
  return buf;
}

bool isNewerInstalledAt(const std::string& a, const std::string& b) {
  if (a.empty() && b.empty()) {
    return false;
  }
  if (a.empty()) {
    return false;
  }
  if (b.empty()) {
    return true;
  }
  return a > b;
}

}  // namespace AppDateTimeFormat
