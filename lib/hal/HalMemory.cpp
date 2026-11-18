#include "HalMemory.h"

#include <esp_heap_caps.h>

#include <cstdint>

namespace {
HalMemory::HeapStats readHeapStats(uint32_t capabilities) {
  return {heap_caps_get_free_size(capabilities), heap_caps_get_total_size(capabilities),
          heap_caps_get_minimum_free_size(capabilities), heap_caps_get_largest_free_block(capabilities)};
}
}  // namespace

HalMemory::HeapStats HalMemory::getDefaultHeap() { return readHeapStats(MALLOC_CAP_DEFAULT); }

HalMemory::HeapStats HalMemory::getInternalHeap() { return readHeapStats(MALLOC_CAP_INTERNAL); }

HalMemory::HeapStats HalMemory::getPsramHeap() { return readHeapStats(MALLOC_CAP_SPIRAM); }

void HalMemory::PsramDeleter::operator()(uint8_t* buffer) const { heap_caps_free(buffer); }

HalMemory::PsramBuffer HalMemory::allocatePsram(size_t bytes) {
  // Capability allocation is required to keep image caches out of internal RAM;
  // the owning handle releases this block through the matching heap API.
  return PsramBuffer(static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
