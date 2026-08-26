#pragma once

#include <cstddef>
#include <cstdint>

namespace FrontlightSchedule {

constexpr uint8_t SLOTS_PER_HOUR = 4;
constexpr uint8_t SLOT_MINUTES = 15;
constexpr uint8_t SLOT_COUNT = 24 * SLOTS_PER_HOUR;

// Start is inclusive and end is exclusive. Equal times intentionally mean an
// all-day window; the separate enable toggle is the unambiguous off switch.
constexpr bool isActiveAtMinute(const uint16_t localMinute, const uint8_t startSlot, const uint8_t endSlot) {
  const uint16_t minute = localMinute % (24U * 60U);
  const uint16_t start = static_cast<uint16_t>(startSlot % SLOT_COUNT) * SLOT_MINUTES;
  const uint16_t end = static_cast<uint16_t>(endSlot % SLOT_COUNT) * SLOT_MINUTES;
  if (start == end) return true;
  return start < end ? minute >= start && minute < end : minute >= start || minute < end;
}

constexpr uint16_t toLocalMinute(const uint8_t utcHour, const uint8_t utcMinute,
                                 const uint8_t utcOffsetQuarterHoursBiased) {
  const int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int minute = static_cast<int>(utcHour) * 60 + static_cast<int>(utcMinute) + offsetQuarterHours * SLOT_MINUTES;
  minute = ((minute % (24 * 60)) + (24 * 60)) % (24 * 60);
  return static_cast<uint16_t>(minute);
}

void formatTimeSlot(uint8_t slot, bool use12Hour, char* buffer, size_t bufferSize);

// Returns false while scheduling is disabled or the RTC has not been synced.
// On success, shouldBeOn is the desired state for the current local time.
bool currentState(bool& shouldBeOn);

}  // namespace FrontlightSchedule
