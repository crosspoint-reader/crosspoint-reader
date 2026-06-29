#pragma once

#ifdef SIMULATOR

#include <cstddef>
#include <cstdint>

namespace SimulatorHeap {

class HostHeapScope {
 public:
  HostHeapScope();
  ~HostHeapScope();

  HostHeapScope(const HostHeapScope&) = delete;
  HostHeapScope& operator=(const HostHeapScope&) = delete;
};

void initializeFromEnv();
void activateFromEnv();
bool isInitialized();
bool isActive();
std::size_t totalBytes();
std::size_t currentUsedBytes();
std::size_t peakUsedBytes();
std::size_t freeBytes();
std::size_t minFreeBytes();
std::size_t largestFreeBlockBytes();
std::size_t fragmentationPercent();

// Caller tracking (enabled by CROSSPOINT_SIM_HEAP_TRACE=1). When on, every arena
// allocation records its call stack; dumpLiveAllocations() prints the live
// allocations aggregated by call site, largest first. Also fired automatically
// on an arena OOM. No-op when tracing is disabled or unavailable on the host.
bool isTraceEnabled();
void dumpLiveAllocations(const char* reason);
void dumpFreeList(const char* reason);
void dumpVisualization(const char* reason);

inline bool isLimited() { return totalBytes() > 0; }
inline std::size_t heapLimitBytes() { return totalBytes(); }

#ifdef SIMULATOR_HEAP_TESTING
bool resetForTests(std::size_t arenaBytes);
void shutdownForTests();
void* allocateForTests(std::size_t size);
void* allocateAlignedForTests(std::size_t size, std::size_t alignment);
void* callocForTests(std::size_t nmemb, std::size_t size);
void* reallocForTests(void* ptr, std::size_t size);
void* reallocAlignedForTests(void* ptr, std::size_t size, std::size_t alignment);
void freeForTests(void* ptr);
void* cppNewForTests(std::size_t size);
void* cppNewNoThrowForTests(std::size_t size) noexcept;
void* cppAlignedNewForTests(std::size_t size, std::size_t alignment);
void* cppAlignedNewNoThrowForTests(std::size_t size, std::size_t alignment) noexcept;
void dumpVisualizationForTests(const char* reason);
#endif

}  // namespace SimulatorHeap

#endif
