#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct KoInsightPendingStat {
  std::string path;
  std::string md5;
  int64_t startTime = 0;
  uint32_t durationSec = 0;
  int page = 1;
  int totalPages = 1;
};

namespace KoInsightPendingSessions {

void append(const char* bookPath, int64_t sessionStartUnix, uint32_t durationSec, uint16_t pageOneBased,
            uint16_t chapterTotalPages);

std::vector<KoInsightPendingStat> loadAll();

void clear();

}  // namespace KoInsightPendingSessions
