#include "ClockTimeZones.h"

#include <cstdlib>

namespace {
// POSIX rules are interpreted by newlib's tzset/localtime_r implementation.
// The table is constexpr, so labels and rules remain in flash and selecting a
// zone only persists its one-byte index.
static constexpr ClockTimeZoneInfo TIME_ZONES[CLOCK_TIME_ZONE_COUNT] = {
    {StrId::STR_TZ_UTC, "UTC0", 0},
    {StrId::STR_TZ_PACIFIC_HONOLULU, "HST10", -600},
    {StrId::STR_TZ_AMERICA_ANCHORAGE, "AKST9AKDT,M3.2.0,M11.1.0", -540},
    {StrId::STR_TZ_AMERICA_LOS_ANGELES, "PST8PDT,M3.2.0,M11.1.0", -480},
    {StrId::STR_TZ_AMERICA_PHOENIX, "MST7", -420},
    {StrId::STR_TZ_AMERICA_DENVER, "MST7MDT,M3.2.0,M11.1.0", -420},
    {StrId::STR_TZ_AMERICA_CHICAGO, "CST6CDT,M3.2.0,M11.1.0", -360},
    {StrId::STR_TZ_AMERICA_NEW_YORK, "EST5EDT,M3.2.0,M11.1.0", -300},
    {StrId::STR_TZ_AMERICA_HALIFAX, "AST4ADT,M3.2.0,M11.1.0", -240},
    {StrId::STR_TZ_AMERICA_ST_JOHNS, "NST3:30NDT,M3.2.0,M11.1.0", -210},
    {StrId::STR_TZ_AMERICA_SAO_PAULO, "<-03>3", -180},
    {StrId::STR_TZ_ATLANTIC_SOUTH_GEORGIA, "<-02>2", -120},
    {StrId::STR_TZ_ATLANTIC_AZORES, "AZOT1AZOST,M3.5.0/0,M10.5.0/1", -60},
    {StrId::STR_TZ_EUROPE_LONDON, "GMT0BST,M3.5.0/1,M10.5.0", 0},
    {StrId::STR_TZ_EUROPE_PARIS, "CET-1CEST,M3.5.0,M10.5.0/3", 60},
    {StrId::STR_TZ_EUROPE_HELSINKI, "EET-2EEST,M3.5.0/3,M10.5.0/4", 120},
    {StrId::STR_TZ_AFRICA_JOHANNESBURG, "SAST-2", 120},
    {StrId::STR_TZ_EUROPE_MOSCOW, "MSK-3", 180},
    {StrId::STR_TZ_ASIA_RIYADH, "<+03>-3", 180},
    {StrId::STR_TZ_ASIA_TEHRAN, "<+0330>-3:30", 210},
    {StrId::STR_TZ_ASIA_DUBAI, "<+04>-4", 240},
    {StrId::STR_TZ_ASIA_KABUL, "<+0430>-4:30", 270},
    {StrId::STR_TZ_ASIA_KARACHI, "<+05>-5", 300},
    {StrId::STR_TZ_ASIA_KOLKATA, "IST-5:30", 330},
    {StrId::STR_TZ_ASIA_KATHMANDU, "<+0545>-5:45", 345},
    {StrId::STR_TZ_ASIA_DHAKA, "<+06>-6", 360},
    {StrId::STR_TZ_ASIA_YANGON, "<+0630>-6:30", 390},
    {StrId::STR_TZ_ASIA_BANGKOK, "<+07>-7", 420},
    {StrId::STR_TZ_ASIA_SHANGHAI, "<+08>-8", 480},
    {StrId::STR_TZ_AUSTRALIA_PERTH, "AWST-8", 480},
    {StrId::STR_TZ_ASIA_TOKYO, "JST-9", 540},
    {StrId::STR_TZ_AUSTRALIA_ADELAIDE, "ACST-9:30ACDT,M10.1.0,M4.1.0/3", 570},
    {StrId::STR_TZ_AUSTRALIA_DARWIN, "ACST-9:30", 570},
    {StrId::STR_TZ_AUSTRALIA_BRISBANE, "AEST-10", 600},
    {StrId::STR_TZ_AUSTRALIA_SYDNEY, "AEST-10AEDT,M10.1.0,M4.1.0/3", 600},
    {StrId::STR_TZ_PACIFIC_NOUMEA, "<+11>-11", 660},
    {StrId::STR_TZ_PACIFIC_AUCKLAND, "NZST-12NZDT,M9.5.0,M4.1.0/3", 720},
    {StrId::STR_TZ_PACIFIC_CHATHAM, "CHAST-12:45CHADT,M9.5.0/2:45,M4.1.0/3:45", 765},
    {StrId::STR_TZ_PACIFIC_TONGATAPU, "<+13>-13", 780},
    {StrId::STR_TZ_PACIFIC_KIRITIMATI, "<+14>-14", 840},
};

static_assert(sizeof(TIME_ZONES) / sizeof(TIME_ZONES[0]) == CLOCK_TIME_ZONE_COUNT);
}  // namespace

const ClockTimeZoneInfo& getClockTimeZone(const uint8_t index) {
  return TIME_ZONES[index < CLOCK_TIME_ZONE_COUNT ? index : CLOCK_TZ_UTC];
}

uint8_t clockTimeZoneFromLegacyOffset(uint8_t biasedQuarterHours) {
  if (biasedQuarterHours > 104) biasedQuarterHours = 48;
  const int targetMinutes = (static_cast<int>(biasedQuarterHours) - 48) * 15;

  uint8_t closest = CLOCK_TZ_UTC;
  int closestDistance = abs(targetMinutes);
  for (uint8_t i = 0; i < CLOCK_TIME_ZONE_COUNT; ++i) {
    const int distance = abs(targetMinutes - TIME_ZONES[i].standardOffsetMinutes);
    if (distance < closestDistance) {
      closest = i;
      closestDistance = distance;
    }
  }
  return closest;
}
