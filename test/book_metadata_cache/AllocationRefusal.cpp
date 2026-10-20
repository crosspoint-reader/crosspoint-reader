#include "AllocationRefusal.h"

#include <malloc.h>

#include <cstdlib>
#include <new>

namespace {
// 0: nothing refused.
size_t refuseFrom = 0;
size_t live = 0;
size_t peak = 0;

bool refused(const size_t bytes) { return refuseFrom != 0 && bytes >= refuseFrom; }

void* allocateOrNull(const size_t bytes) {
  if (refused(bytes)) return nullptr;
  void* p = std::malloc(bytes == 0 ? 1 : bytes);
  if (p != nullptr) {
    live += malloc_usable_size(p);
    if (live > peak) peak = live;
  }
  return p;
}

void* allocateOrThrow(const size_t bytes) {
  void* p = allocateOrNull(bytes);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void release(void* p) {
  if (p == nullptr) return;
  live -= malloc_usable_size(p);
  std::free(p);
}
}  // namespace

AllocationRefusal::AllocationRefusal(const size_t bytes) { refuseFrom = bytes; }
AllocationRefusal::~AllocationRefusal() { refuseFrom = 0; }

size_t heapLiveBytes() { return live; }
size_t heapPeakBytes() { return peak; }
void resetHeapPeak() { peak = live; }

// Every allocating form and every releasing form, so the live count sees each block both ways.
void* operator new(const size_t bytes) { return allocateOrThrow(bytes); }
void* operator new[](const size_t bytes) { return allocateOrThrow(bytes); }
void* operator new(const size_t bytes, const std::nothrow_t&) noexcept { return allocateOrNull(bytes); }
void* operator new[](const size_t bytes, const std::nothrow_t&) noexcept { return allocateOrNull(bytes); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
