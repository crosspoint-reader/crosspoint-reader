#include "FrontlightSchedule.h"

#include <HalClock.h>

#include <cstdio>

#include "CrossPointSettings.h"

namespace FrontlightSchedule {

void formatTimeSlot(uint8_t slot, const bool use12Hour, char* buffer, const size_t bufferSize) {
  if (buffer == nullptr || bufferSize == 0) return;

  slot %= SLOT_COUNT;
  const uint8_t hour24 = slot / SLOTS_PER_HOUR;
  const uint8_t minute = (slot % SLOTS_PER_HOUR) * SLOT_MINUTES;
  if (!use12Hour) {
    snprintf(buffer, bufferSize, "%02u:%02u", static_cast<unsigned>(hour24), static_cast<unsigned>(minute));
    return;
  }

  const bool pm = hour24 >= 12;
  uint8_t hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(buffer, bufferSize, "%u:%02u %s", static_cast<unsigned>(hour12), static_cast<unsigned>(minute),
           pm ? "PM" : "AM");
}

bool currentState(bool& shouldBeOn) {
  if (SETTINGS.frontlightScheduleEnabled == 0 || SETTINGS.clockHasBeenSynced == 0 || !halClock.isAvailable()) {
    return false;
  }

  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getTime(hour, minute)) return false;

  const uint8_t utcOffsetQ = SETTINGS.clockUtcOffsetQ <= 104 ? SETTINGS.clockUtcOffsetQ : 104;
  shouldBeOn = isActiveAtMinute(toLocalMinute(hour, minute, utcOffsetQ), SETTINGS.frontlightScheduleStartQ,
                                SETTINGS.frontlightScheduleEndQ);
  return true;
}

}  // namespace FrontlightSchedule
