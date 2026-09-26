#pragma once

// Heap model for the huge-book tests. The firmware is built with -fno-exceptions,
// so a throwing operator new that finds no memory calls abort() and reboots the
// device. Here every allocation made while counting is charged against a byte cap:
// a throwing allocation past the cap is recorded as an abort (and still served, so
// the test can report it), a nothrow allocation past the cap returns nullptr.
// HeapCap.cpp replaces the global operators.
#include <cstddef>
#include <cstdint>

namespace heapcap {

void reset(size_t cap);  // start counting with this many bytes available
void stop();             // stop counting
size_t available();      // bytes left under the cap
size_t live();           // counted bytes currently allocated
size_t peak();           // high-water mark of live() since reset() or resetPeak()
void resetPeak();        // restart the high-water mark from live()
unsigned aborts();       // throwing allocations past the cap
size_t firstAbortSize();

// Test-side storage (the fake SD card, the fake zip) is not device heap.
struct Untracked {
  Untracked();
  ~Untracked();
  Untracked(const Untracked&) = delete;
  Untracked& operator=(const Untracked&) = delete;
};

}  // namespace heapcap
