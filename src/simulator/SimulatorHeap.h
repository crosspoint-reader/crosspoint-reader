#pragma once

#ifdef SIMULATOR

#include <cstddef>
#include <cstdint>

namespace SimulatorHeap {

void initializeFromEnv();
bool isLimited();
std::size_t heapLimitBytes();
std::size_t currentUsedBytes();
std::size_t peakUsedBytes();
std::size_t freeBytes();
std::size_t minFreeBytes();

}  // namespace SimulatorHeap

#endif
