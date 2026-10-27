#pragma once

#include <cstddef>
#include <cstdint>

// Night mode schedule: an on-time and an off-time of day, in half-hour slots.
//
// The schedule sets night mode only when a boundary passes, so a manual change
// holds until the next one. Callers remember the last boundary they applied and
// act only when latestBoundary() returns a different one.
namespace nightschedule {

// Half-hour slots in a day; slot 44 is 22:00.
constexpr uint8_t SLOT_COUNT = 48;

struct Boundary {
  int64_t at;    // epoch seconds of the boundary; 0 when there is none
  bool nightOn;  // true at an on-time, false at an off-time
};

// The most recent on- or off-time at or before `now`, worked out in local time
// (the process TZ), so daylight-saving changes move it with the wall clock.
// Returns {0, false} when the two times are equal or a slot is out of range.
Boundary latestBoundary(int64_t now, uint8_t onSlot, uint8_t offSlot);

// A slot as a clock time: "22:00", or "10:00 PM" when twelveHour is set.
void formatSlot(uint8_t slot, bool twelveHour, char* buf, size_t size);

}  // namespace nightschedule
