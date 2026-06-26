#ifdef SIMULATOR

#include "SimulatorHeap.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {

struct alignas(std::max_align_t) AllocationHeader {
  static constexpr std::uint64_t kMagic = 0x4350484541503031ULL;
  std::uint64_t magic;
  std::size_t requested;
};

std::atomic<std::size_t> gHeapLimitBytes{0};
std::atomic<std::size_t> gCurrentUsedBytes{0};
std::atomic<std::size_t> gPeakUsedBytes{0};
std::atomic<bool> gInitialized{false};

bool reserveBytes(const std::size_t bytes) {
  const std::size_t limit = gHeapLimitBytes.load(std::memory_order_relaxed);
  std::size_t current = gCurrentUsedBytes.load(std::memory_order_relaxed);
  while (true) {
    if (limit > 0 && (bytes > limit || current > limit - bytes)) {
      errno = ENOMEM;
      return false;
    }
    if (gCurrentUsedBytes.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel)) {
      std::size_t peak = gPeakUsedBytes.load(std::memory_order_relaxed);
      while (peak < current + bytes &&
             !gPeakUsedBytes.compare_exchange_weak(peak, current + bytes, std::memory_order_acq_rel)) {
      }
      return true;
    }
  }
}

void releaseBytes(const std::size_t bytes) {
  if (bytes == 0) return;
  gCurrentUsedBytes.fetch_sub(bytes, std::memory_order_acq_rel);
}

AllocationHeader* toHeader(void* userPtr) {
  return static_cast<AllocationHeader*>(userPtr) - 1;
}

void* fromHeader(AllocationHeader* header) { return static_cast<void*>(header + 1); }

std::size_t allocationSize(void* userPtr) {
  if (!userPtr) return 0;
  AllocationHeader* header = toHeader(userPtr);
  if (header->magic != AllocationHeader::kMagic) return 0;
  return header->requested;
}

std::size_t parseByteEnv(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return 0;

  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(raw, &end, 10);
  if (errno != 0 || !end || *end != '\0') {
    std::fprintf(stderr, "[SIM] invalid %s=%s, ignoring heap limit\n", name, raw);
    return 0;
  }
  if (parsed > std::numeric_limits<std::size_t>::max()) {
    std::fprintf(stderr, "[SIM] %s=%s exceeds size_t range, ignoring heap limit\n", name, raw);
    return 0;
  }
  return static_cast<std::size_t>(parsed);
}

}  // namespace

namespace SimulatorHeap {

void initializeFromEnv() {
  bool expected = false;
  if (!gInitialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

  const std::size_t heapBytes = parseByteEnv("CROSSPOINT_SIM_HEAP_BYTES");
  gHeapLimitBytes.store(heapBytes, std::memory_order_release);

  if (heapBytes > 0) {
    std::fprintf(stderr, "[SIM] heap limit enabled: %zu bytes\n", heapBytes);
  }
}

bool isLimited() { return heapLimitBytes() > 0; }

std::size_t heapLimitBytes() { return gHeapLimitBytes.load(std::memory_order_acquire); }

std::size_t currentUsedBytes() { return gCurrentUsedBytes.load(std::memory_order_acquire); }

std::size_t peakUsedBytes() { return gPeakUsedBytes.load(std::memory_order_acquire); }

std::size_t freeBytes() {
  const std::size_t limit = heapLimitBytes();
  if (limit == 0) return 0;
  const std::size_t used = currentUsedBytes();
  return used >= limit ? 0 : limit - used;
}

std::size_t minFreeBytes() {
  const std::size_t limit = heapLimitBytes();
  if (limit == 0) return 0;
  const std::size_t peak = peakUsedBytes();
  return peak >= limit ? 0 : limit - peak;
}

}  // namespace SimulatorHeap

extern "C" void* __real_malloc(std::size_t size);
extern "C" void* __real_calloc(std::size_t nmemb, std::size_t size);
extern "C" void* __real_realloc(void* ptr, std::size_t size);
extern "C" void __real_free(void* ptr);
extern "C" void __wrap_free(void* ptr);

extern "C" void* __wrap_malloc(std::size_t size) {
  SimulatorHeap::initializeFromEnv();
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    errno = ENOMEM;
    return nullptr;
  }
  if (!reserveBytes(size)) return nullptr;

  AllocationHeader* header = static_cast<AllocationHeader*>(__real_malloc(sizeof(AllocationHeader) + size));
  if (!header) {
    releaseBytes(size);
    return nullptr;
  }
  header->magic = AllocationHeader::kMagic;
  header->requested = size;
  return fromHeader(header);
}

extern "C" void* __wrap_calloc(std::size_t nmemb, std::size_t size) {
  SimulatorHeap::initializeFromEnv();
  if (nmemb != 0 && size > std::numeric_limits<std::size_t>::max() / nmemb) {
    errno = ENOMEM;
    return nullptr;
  }
  const std::size_t requested = nmemb * size;
  if (requested > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    errno = ENOMEM;
    return nullptr;
  }
  if (!reserveBytes(requested)) return nullptr;

  AllocationHeader* header =
      static_cast<AllocationHeader*>(__real_calloc(1, sizeof(AllocationHeader) + requested));
  if (!header) {
    releaseBytes(requested);
    return nullptr;
  }
  header->magic = AllocationHeader::kMagic;
  header->requested = requested;
  return fromHeader(header);
}

extern "C" void* __wrap_realloc(void* ptr, std::size_t size) {
  SimulatorHeap::initializeFromEnv();
  if (!ptr) return __wrap_malloc(size);
  if (size == 0) {
    __wrap_free(ptr);
    return nullptr;
  }

  AllocationHeader* oldHeader = toHeader(ptr);
  if (oldHeader->magic != AllocationHeader::kMagic) {
    return __real_realloc(ptr, size);
  }

  const std::size_t oldSize = oldHeader->requested;
  if (size > oldSize && !reserveBytes(size - oldSize)) return nullptr;

  if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    if (size > oldSize) releaseBytes(size - oldSize);
    errno = ENOMEM;
    return nullptr;
  }

  AllocationHeader* newHeader =
      static_cast<AllocationHeader*>(__real_realloc(oldHeader, sizeof(AllocationHeader) + size));
  if (!newHeader) {
    if (size > oldSize) releaseBytes(size - oldSize);
    return nullptr;
  }

  newHeader->magic = AllocationHeader::kMagic;
  newHeader->requested = size;
  if (oldSize > size) releaseBytes(oldSize - size);
  return fromHeader(newHeader);
}

extern "C" void __wrap_free(void* ptr) {
  if (!ptr) return;

  AllocationHeader* header = toHeader(ptr);
  if (header->magic == AllocationHeader::kMagic) {
    const std::size_t requested = header->requested;
    header->magic = 0;
    releaseBytes(requested);
    __real_free(header);
    return;
  }
  __real_free(ptr);
}

void* operator new(std::size_t size) {
  if (void* ptr = __wrap_malloc(size)) return ptr;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  if (void* ptr = __wrap_malloc(size)) return ptr;
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return __wrap_malloc(size); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return __wrap_malloc(size); }

void operator delete(void* ptr) noexcept { __wrap_free(ptr); }

void operator delete[](void* ptr) noexcept { __wrap_free(ptr); }

void operator delete(void* ptr, std::size_t) noexcept { __wrap_free(ptr); }

void operator delete[](void* ptr, std::size_t) noexcept { __wrap_free(ptr); }

#endif
