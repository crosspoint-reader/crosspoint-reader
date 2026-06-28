#pragma once

#ifdef SIMULATOR

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SimulatorHeapViz {

enum ThresholdFlags : unsigned {
  kNoThresholdChange = 0,
  kPeakUsedIncreased = 1U << 0,
  kLargestFreeBlockDecreased = 1U << 1,
};

struct AllocationSnapshot {
  std::size_t offset = 0;
  std::size_t payloadBytes = 0;
  std::size_t headerBytes = 0;
  std::size_t requestedBytes = 0;
  std::uintptr_t pointerValue = 0;
  std::uint64_t siteHash = 0;
  std::string titleText;
  std::string detailHeaderText;
  std::string stackText;
};

struct Snapshot {
  std::size_t arenaBytes = 0;
  std::size_t controlBytes = 0;
  std::size_t sentinelBytes = 0;
  std::size_t freeBytes = 0;
  std::size_t peakUsedBytes = 0;
  std::size_t minFreeBytes = 0;
  std::size_t largestFreeBlockBytes = 0;
  std::size_t fragmentationPercent = 0;
  std::string contextTitle;
  std::string contextText;
  std::vector<AllocationSnapshot> allocations;
};

void configureFromEnv();
bool enabled();
unsigned updateThresholdState(std::size_t peakUsedBytes, std::size_t largestFreeBlockBytes);
bool writeSnapshot(const Snapshot& snapshot, const char* reason);

#ifdef SIMULATOR_HEAP_TESTING
std::string outputDirForTests();
#endif

}  // namespace SimulatorHeapViz

#endif
