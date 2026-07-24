#include "ClockCalendar.h"

#include <limits>

namespace ClockCalendar {
namespace {

bool leap(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && leap(year) ? 29 : DAYS[month - 1];
}

// Days since 1970-01-01. Algorithm by Howard Hinnant, used here to avoid a
// timezone-dependent mktime() during cold boot.
int64_t daysFromCivil(int64_t year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

}  // namespace

bool isValid(const DateTime& value) {
  return value.year >= 2000 && value.year <= 2099 && value.month >= 1 && value.month <= 12 && value.day >= 1 &&
         value.day <= daysInMonth(value.year, value.month) && value.hour < 24 && value.minute < 60 &&
         value.second < 60;
}

bool toEpoch(const DateTime& value, time_t& epoch) {
  if (!isValid(value)) return false;
  const int64_t seconds = daysFromCivil(value.year, value.month, value.day) * 86400 + value.hour * 3600 +
                          value.minute * 60 + value.second;
  if (seconds < 0 || static_cast<uint64_t>(seconds) > std::numeric_limits<time_t>::max()) return false;
  epoch = static_cast<time_t>(seconds);
  return true;
}

bool fromEpoch(const time_t epoch, DateTime& value) {
  if (!isValidSystemEpoch(epoch)) return false;
  struct tm utc{};
  if (!gmtime_r(&epoch, &utc)) return false;
  value = {static_cast<uint16_t>(utc.tm_year + 1900), static_cast<uint8_t>(utc.tm_mon + 1),
           static_cast<uint8_t>(utc.tm_mday),         static_cast<uint8_t>(utc.tm_hour),
           static_cast<uint8_t>(utc.tm_min),          static_cast<uint8_t>(utc.tm_sec),
           static_cast<uint8_t>(utc.tm_wday)};
  return isValid(value);
}

bool isValidSystemEpoch(const time_t epoch) {
  constexpr int64_t MIN_VALID_TIME = 946684800;   // 2000-01-01 UTC
  constexpr int64_t MAX_VALID_TIME = 4102444799;  // 2099-12-31 23:59:59 UTC
  const int64_t seconds = static_cast<int64_t>(epoch);
  return seconds >= MIN_VALID_TIME && seconds <= MAX_VALID_TIME;
}

}  // namespace ClockCalendar
