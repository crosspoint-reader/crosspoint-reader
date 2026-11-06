#pragma once

#include <cstdint>

// Runtime steps below the public API, for the port and the tests.
namespace bleturner::detail {

// One start on the start task (port::spawnStart). False when one is already in flight or
// the task could not be made; the result shows in status() later.
bool startAsync();
// Stops the radio and keeps "stopped for idleness" as the reason until a key or the user
// starts it again.
bool stopForIdle();
// Cancels a start not yet run and stops the stack, the SDK waiting up to endTimeoutMs.
// False while a start is in flight or the stack is still stopping.
bool suspend(uint32_t endTimeoutMs);
bool heapRestartWanted();

#ifdef BLETURNER_TESTING
// Back to a fresh boot (the RTC memo excepted).
void resetForTests();
#endif

}  // namespace bleturner::detail
