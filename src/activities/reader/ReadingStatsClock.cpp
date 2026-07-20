#include <algorithm>
#include <ctime>

#include "CrossPointSettings.h"
#include "ReadingStatsUtils.h"

bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime) {
  outDateTime = {};
  const time_t utcNow = time(nullptr);
  const int64_t utcSeconds = static_cast<int64_t>(utcNow);
  constexpr int64_t MIN_VALID_TIME = 946684800;   // 2000-01-01 00:00:00 UTC
  constexpr int64_t MAX_VALID_TIME = 4102444799;  // 2099-12-31 23:59:59 UTC
  if (utcSeconds < MIN_VALID_TIME || utcSeconds > MAX_VALID_TIME) return false;

  const int offsetQuarterHours = static_cast<int>(std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104)) - 48;
  const time_t localNow = utcNow + static_cast<time_t>(offsetQuarterHours * 15 * 60);
  struct tm localTime{};
  if (!gmtime_r(&localNow, &localTime)) return false;

  outDateTime.date = {static_cast<uint16_t>(localTime.tm_year + 1900), static_cast<uint8_t>(localTime.tm_mon + 1),
                      static_cast<uint8_t>(localTime.tm_mday)};
  outDateTime.hour = static_cast<uint8_t>(localTime.tm_hour);
  outDateTime.minute = static_cast<uint8_t>(localTime.tm_min);
  outDateTime.second = static_cast<uint8_t>(localTime.tm_sec);
  if (!outDateTime.isValid()) outDateTime = {};
  return outDateTime.isValid();
}
