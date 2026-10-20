#pragma once

#include <cstddef>

// Test-only stand-in for a fragmented device heap. While a refusal is armed, every allocation of
// at least `bytes` fails: the nothrow forms return nullptr, the throwing forms throw std::bad_alloc
// (where the device, built -fno-exceptions, would abort()). Smaller allocations are unaffected.
//
// The replacement allocation functions live in AllocationRefusal.cpp and apply to the whole test
// binary, so arm the refusal only around the code under test.
class AllocationRefusal {
 public:
  explicit AllocationRefusal(size_t bytes);
  ~AllocationRefusal();
  AllocationRefusal(const AllocationRefusal&) = delete;
  AllocationRefusal& operator=(const AllocationRefusal&) = delete;
};

// What the same replacement functions have handed out through operator new and not taken back
// (allocator granularity included): live now, and the most live since resetHeapPeak(). Lets a test
// bound the heap a piece of code holds at once, where the device would run out.
size_t heapLiveBytes();
size_t heapPeakBytes();
void resetHeapPeak();
