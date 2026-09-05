#pragma once

#include <cstdint>

// A monotonic floor under the system clock, for loan-expiry enforcement.
//
// The ESP32-C3 keeps time through deep sleep but loses it on power-off, and
// the device has no battery-backed RTC — so a cold boot starts near epoch 0
// and a date-based check would never fire offline. This module persists the
// last known-good time in NVS (on-flash, not on the removable SD card) and
// restores it into the system clock at boot, so time only ever moves forward
// across power cycles. Real time snaps in whenever Wi-Fi is up via SNTP.
//
// The floor can lag real time while the device sits powered off; it can never
// run behind a moment the device has already seen. Enforcement that needs a
// trustworthy "now" uses trustedNow() and fails closed when it returns 0.
namespace trustedtime {

// Restore the persisted floor into the system clock. Call once at boot,
// before anything reads time().
void init();

// Persist the floor when the clock advanced past it. Cheap (one NVS read,
// a write only when it moved). Call at sleep entry and after a time sync.
void note();

// Kick off a non-blocking SNTP sync; the sync callback persists the floor.
// Call whenever a Wi-Fi station connection comes up. No-op while running.
void startSync();

// Blocking SNTP sync with a deadline, for callers that need the wall clock
// right now (e.g. progress-sync timestamps). Returns true when synced.
bool syncNow(uint32_t timeoutMs);

// Epoch seconds when the clock is trustworthy (a plausible present-day
// value, restored or synced), else 0. Callers enforcing a date fail closed
// on 0.
int64_t trustedNow();

}  // namespace trustedtime
