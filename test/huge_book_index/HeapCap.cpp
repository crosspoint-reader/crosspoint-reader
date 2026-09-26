#include "HeapCap.h"

#include <cstdlib>
#include <new>

namespace {
bool counting = false;
size_t capBytes = SIZE_MAX;
size_t liveBytes = 0;
size_t peakBytes = 0;
unsigned abortCount = 0;
size_t firstAbort = 0;
int untrackedDepth = 0;

struct Header {
  size_t size;
  size_t counted;
  size_t pad[2];
};
static_assert(sizeof(Header) % alignof(std::max_align_t) == 0, "header keeps the payload aligned");

void* allocate(const size_t size, const bool nothrow) {
  const bool count = counting && untrackedDepth == 0;
  if (count && size > heapcap::available()) {
    if (nothrow) return nullptr;
    if (abortCount++ == 0) firstAbort = size;
  }
  auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + (size ? size : 1)));
  if (!header) {
    if (nothrow) return nullptr;
    std::abort();
  }
  header->size = size;
  header->counted = count ? 1 : 0;
  if (count) {
    liveBytes += size;
    if (liveBytes > peakBytes) peakBytes = liveBytes;
  }
  return header + 1;
}

void release(void* p) {
  if (!p) return;
  auto* header = static_cast<Header*>(p) - 1;
  if (header->counted) liveBytes -= header->size;
  std::free(header);
}
}  // namespace

namespace heapcap {
void reset(const size_t cap) {
  capBytes = cap;
  liveBytes = peakBytes = 0;
  abortCount = 0;
  firstAbort = 0;
  counting = true;
}
void stop() { counting = false; }
size_t available() { return liveBytes >= capBytes ? 0 : capBytes - liveBytes; }
size_t live() { return liveBytes; }
size_t peak() { return peakBytes; }
void resetPeak() { peakBytes = liveBytes; }
unsigned aborts() { return abortCount; }
size_t firstAbortSize() { return firstAbort; }
Untracked::Untracked() { ++untrackedDepth; }
Untracked::~Untracked() { --untrackedDepth; }
}  // namespace heapcap

void* operator new(std::size_t size) { return allocate(size, false); }
void* operator new[](std::size_t size) { return allocate(size, false); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size, true); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size, true); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
