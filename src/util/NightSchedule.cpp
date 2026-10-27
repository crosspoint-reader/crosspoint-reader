#include "NightSchedule.h"

#include <cstdio>
#include <ctime>

namespace nightschedule {
namespace {

// The slot's local time on the day `dayOffset` days from `today`. mktime
// normalizes the out-of-range day and resolves DST (tm_isdst = -1).
int64_t slotTime(const tm& today, int dayOffset, uint8_t slot) {
  tm t = today;
  t.tm_mday += dayOffset;
  t.tm_hour = slot / 2;
  t.tm_min = (slot % 2) * 30;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&t));
}

}  // namespace

Boundary latestBoundary(int64_t now, uint8_t onSlot, uint8_t offSlot) {
  if (onSlot == offSlot || onSlot >= SLOT_COUNT || offSlot >= SLOT_COUNT) return {0, false};

  const time_t nowT = static_cast<time_t>(now);
  tm today{};
  localtime_r(&nowT, &today);

  // Each time happens once a day, so the latest one is today's or yesterday's.
  Boundary best{0, false};
  for (int dayOffset = -1; dayOffset <= 0; ++dayOffset) {
    const int64_t on = slotTime(today, dayOffset, onSlot);
    const int64_t off = slotTime(today, dayOffset, offSlot);
    if (on <= now && on > best.at) best = {on, true};
    if (off <= now && off > best.at) best = {off, false};
  }
  return best;
}

void formatSlot(uint8_t slot, bool twelveHour, char* buf, size_t size) {
  const int hour = slot / 2;
  const int minute = (slot % 2) * 30;
  if (twelveHour) {
    snprintf(buf, size, "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
  } else {
    snprintf(buf, size, "%02d:%02d", hour, minute);
  }
}

}  // namespace nightschedule
