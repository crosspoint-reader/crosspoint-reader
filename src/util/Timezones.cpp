#include "Timezones.h"

#include <HalClock.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"

namespace timezones {
namespace {

// Ordered west to east by standard offset. APPEND ONLY (see header).
constexpr TimezoneInfo TABLE[] = {
    {"Midway", "SST11", -44},
    {"Honolulu", "HST10", -40},
    {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0", -36},
    {"Los Angeles / Vancouver", "PST8PDT,M3.2.0,M11.1.0", -32},
    {"Denver / Edmonton", "MST7MDT,M3.2.0,M11.1.0", -28},
    {"Phoenix", "MST7", -28},
    {"Chicago / Winnipeg", "CST6CDT,M3.2.0,M11.1.0", -24},
    {"Mexico City", "CST6", -24},
    {"New York / Toronto", "EST5EDT,M3.2.0,M11.1.0", -20},
    {"Bogota / Lima", "COT5", -20},
    {"Halifax", "AST4ADT,M3.2.0,M11.1.0", -16},
    {"Caracas / La Paz", "VET4", -16},
    {"Santiago", "CLT4CLST,M9.1.6/24,M4.1.6/24", -16},
    {"St. John's", "NST3:30NDT,M3.2.0,M11.1.0", -14},
    {"Buenos Aires / Montevideo", "ART3", -12},
    {"Sao Paulo", "BRT3", -12},
    {"Azores", "AZOT1AZOST,M3.5.0/0,M10.5.0/1", -4},
    {"UTC", "UTC0", 0},
    {"London / Dublin / Lisbon", "GMT0BST,M3.5.0/1,M10.5.0", 0},
    {"Berlin / Paris / Madrid / Rome", "CET-1CEST,M3.5.0,M10.5.0/3", 4},
    {"Lagos / Algiers", "WAT-1", 4},
    {"Athens / Helsinki / Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4", 8},
    {"Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24", 8},
    {"Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0", 8},
    {"Johannesburg", "SAST-2", 8},
    {"Moscow / Istanbul / Riyadh", "MSK-3", 12},
    {"Nairobi", "EAT-3", 12},
    {"Tehran", "IRST-3:30", 14},
    {"Dubai / Tbilisi", "GST-4", 16},
    {"Kabul", "AFT-4:30", 18},
    {"Karachi / Tashkent", "PKT-5", 20},
    {"India / Colombo", "IST-5:30", 22},
    {"Kathmandu", "NPT-5:45", 23},
    {"Dhaka / Almaty", "BST-6", 24},
    {"Yangon", "MMT-6:30", 26},
    {"Bangkok / Jakarta / Hanoi", "ICT-7", 28},
    {"China / Hong Kong / Taipei", "CST-8", 32},
    {"Singapore / Manila / Kuala Lumpur", "SGT-8", 32},
    {"Perth", "AWST-8", 32},
    {"Tokyo / Seoul", "JST-9", 36},
    {"Darwin", "ACST-9:30", 38},
    {"Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", 38},
    {"Brisbane / Guam", "AEST-10", 40},
    {"Sydney / Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3", 40},
    {"Honiara / Noumea", "SBT-11", 44},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", 48},
    {"Fiji", "FJT-12", 48},
    {"Nuku'alofa", "TOT-13", 52},
    {"Kiritimati", "LINT-14", 56},
    // Fixed offsets, no DST — the manual fallback for zones the named list
    // misses. Note the POSIX offset sign is inverted relative to the label.
    {"UTC-12:00", "UTC12", -48},
    {"UTC-11:30", "UTC11:30", -46},
    {"UTC-11:00", "UTC11", -44},
    {"UTC-10:30", "UTC10:30", -42},
    {"UTC-10:00", "UTC10", -40},
    {"UTC-09:30", "UTC9:30", -38},
    {"UTC-09:00", "UTC9", -36},
    {"UTC-08:30", "UTC8:30", -34},
    {"UTC-08:00", "UTC8", -32},
    {"UTC-07:30", "UTC7:30", -30},
    {"UTC-07:00", "UTC7", -28},
    {"UTC-06:30", "UTC6:30", -26},
    {"UTC-06:00", "UTC6", -24},
    {"UTC-05:30", "UTC5:30", -22},
    {"UTC-05:00", "UTC5", -20},
    {"UTC-04:30", "UTC4:30", -18},
    {"UTC-04:00", "UTC4", -16},
    {"UTC-03:30", "UTC3:30", -14},
    {"UTC-03:00", "UTC3", -12},
    {"UTC-02:30", "UTC2:30", -10},
    {"UTC-02:00", "UTC2", -8},
    {"UTC-01:30", "UTC1:30", -6},
    {"UTC-01:00", "UTC1", -4},
    {"UTC-00:30", "UTC0:30", -2},
    {"UTC+00:30", "UTC-0:30", 2},
    {"UTC+01:00", "UTC-1", 4},
    {"UTC+01:30", "UTC-1:30", 6},
    {"UTC+02:00", "UTC-2", 8},
    {"UTC+02:30", "UTC-2:30", 10},
    {"UTC+03:00", "UTC-3", 12},
    {"UTC+03:30", "UTC-3:30", 14},
    {"UTC+04:00", "UTC-4", 16},
    {"UTC+04:30", "UTC-4:30", 18},
    {"UTC+05:00", "UTC-5", 20},
    {"UTC+05:30", "UTC-5:30", 22},
    {"UTC+05:45", "UTC-5:45", 23},
    {"UTC+06:00", "UTC-6", 24},
    {"UTC+06:30", "UTC-6:30", 26},
    {"UTC+07:00", "UTC-7", 28},
    {"UTC+07:30", "UTC-7:30", 30},
    {"UTC+08:00", "UTC-8", 32},
    {"UTC+08:30", "UTC-8:30", 34},
    {"UTC+08:45", "UTC-8:45", 35},
    {"UTC+09:00", "UTC-9", 36},
    {"UTC+09:30", "UTC-9:30", 38},
    {"UTC+10:00", "UTC-10", 40},
    {"UTC+10:30", "UTC-10:30", 42},
    {"UTC+11:00", "UTC-11", 44},
    {"UTC+11:30", "UTC-11:30", 46},
    {"UTC+12:00", "UTC-12", 48},
    {"UTC+12:30", "UTC-12:30", 50},
    {"UTC+12:45", "UTC-12:45", 51},
    {"UTC+13:00", "UTC-13", 52},
    {"UTC+13:30", "UTC-13:30", 54},
    {"UTC+14:00", "UTC-14", 56},
};
constexpr size_t TABLE_COUNT = sizeof(TABLE) / sizeof(TABLE[0]);
constexpr uint8_t UTC_INDEX = 17;
static_assert(TABLE[UTC_INDEX].stdOffsetQ == 0, "UTC_INDEX must point at the UTC entry");
static_assert(TABLE_COUNT < 255, "255 is the 'never chosen' sentinel in clockTimezone");

}  // namespace

const TimezoneInfo* table() { return TABLE; }
size_t count() { return TABLE_COUNT; }
uint8_t utcIndex() { return UTC_INDEX; }

uint8_t activeIndex() {
  if (SETTINGS.clockTimezone < TABLE_COUNT) return SETTINGS.clockTimezone;
  // Legacy migration: the retired quarter-hour setting was a FIXED offset,
  // and users in DST regions set their current wall offset (the daylight one,
  // half the year), so it must map to a fixed "UTC±HH:MM" entry — matching a
  // region by standard offset could land a summer-configured device one zone
  // over and shift the clock. A real zone is chosen in the picker.
  if (SETTINGS.clockUtcOffsetQ <= 104 && SETTINGS.clockUtcOffsetQ != 48) {
    const int legacyQ = static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48;
    for (size_t i = 0; i < TABLE_COUNT; i++) {
      if (TABLE[i].stdOffsetQ == legacyQ && strncmp(TABLE[i].name, "UTC", 3) == 0) return static_cast<uint8_t>(i);
    }
    // No fixed entry for this offset (only possible for future named-only
    // appends): first match keeps the clock closest to the old value.
    for (size_t i = 0; i < TABLE_COUNT; i++) {
      if (TABLE[i].stdOffsetQ == legacyQ) return static_cast<uint8_t>(i);
    }
  }
  return UTC_INDEX;
}

void formatOffset(const uint8_t index, char* buf, const size_t bufSize) {
  if (index >= TABLE_COUNT) {
    snprintf(buf, bufSize, "UTC");
    return;
  }
  const int q = TABLE[index].stdOffsetQ;
  if (q == 0) {
    snprintf(buf, bufSize, "UTC");
    return;
  }
  const int absQ = q < 0 ? -q : q;
  snprintf(buf, bufSize, "UTC%c%d:%02d", q < 0 ? '-' : '+', absQ / 4, (absQ % 4) * 15);
}

void applyToClock() {
  const TimezoneInfo& tz = TABLE[activeIndex()];
  if (SETTINGS.clockDst == CrossPointSettings::CLOCK_DST_AUTO) {
    halClock.setTimezone(tz.posixTz);
    return;
  }
  // Forced DST: replace the zone's rule with a fixed offset — the standard
  // offset, plus one hour when forced on. POSIX offset sign is inverted.
  const int q = tz.stdOffsetQ + (SETTINGS.clockDst == CrossPointSettings::CLOCK_DST_ON ? 4 : 0);
  const int absQ = q < 0 ? -q : q;
  char fixed[16];
  snprintf(fixed, sizeof(fixed), "UTC%c%d:%02d", q < 0 ? '+' : '-', absQ / 4, (absQ % 4) * 15);
  halClock.setTimezone(fixed);
}

}  // namespace timezones
