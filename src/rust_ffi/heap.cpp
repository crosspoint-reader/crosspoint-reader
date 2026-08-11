// Heap figures.
//
// Rust allocates through this same heap, so these cover both languages. They
// are the only visibility into what Rust costs at run time: the build-time size
// report measures static sections, where Rust contributes almost nothing.

#include <Arduino.h>

#include "internal.h"

extern "C" {

// The whole heap, so a figure can be shown as used-of-available rather than a
// bare byte count. This is DRAM the allocator owns, which is already smaller
// than the chip's SRAM: the static image comes off first.
int32_t cpp_heap_total() { return static_cast<int32_t>(ESP.getHeapSize()); }

int32_t cpp_heap_free() { return static_cast<int32_t>(ESP.getFreeHeap()); }

int32_t cpp_heap_largest_block() { return static_cast<int32_t>(ESP.getMaxAllocHeap()); }

int32_t cpp_heap_min_free() { return static_cast<int32_t>(ESP.getMinFreeHeap()); }
}
