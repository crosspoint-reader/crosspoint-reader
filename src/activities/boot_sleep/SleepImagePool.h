#pragma once

#include <cstddef>
#include <cstdint>

namespace SleepImagePool {

struct IndexLocation {
  size_t directoryIndex = 0;
  uint16_t localIndex = 0;
  bool valid = false;
};

inline IndexLocation resolveIndex(const uint16_t* directoryCounts, size_t directoryCount, uint16_t globalIndex) {
  uint32_t remaining = globalIndex;
  for (size_t i = 0; i < directoryCount; i++) {
    if (remaining < directoryCounts[i]) {
      return {i, static_cast<uint16_t>(remaining), true};
    }
    remaining -= directoryCounts[i];
  }
  return {};
}

}  // namespace SleepImagePool
