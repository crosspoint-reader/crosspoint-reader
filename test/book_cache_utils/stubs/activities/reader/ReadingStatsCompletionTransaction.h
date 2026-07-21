#pragma once

#include <string>
#include <utility>

namespace ReadingStatsCompletionTransaction {
inline std::string blockedCachePath;

inline bool canRelocateOrDeleteEpubCache(const std::string& cachePath) {
  return blockedCachePath.empty() || blockedCachePath != cachePath;
}

inline void blockCacheForTest(std::string cachePath) { blockedCachePath = std::move(cachePath); }
inline void clearBlockedCacheForTest() { blockedCachePath.clear(); }
}  // namespace ReadingStatsCompletionTransaction
