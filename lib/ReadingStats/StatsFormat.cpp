#include "StatsFormat.h"

#include <cstdio>

namespace reading_stats {

std::string formatDurationMs(uint32_t ms) {
  const uint32_t totalSec = ms / 1000;
  const uint32_t hours = totalSec / 3600;
  const uint32_t mins = (totalSec % 3600) / 60;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%uh %um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
  } else {
    const uint32_t secs = totalSec % 60;
    std::snprintf(buf, sizeof(buf), "%um %us", static_cast<unsigned>(mins), static_cast<unsigned>(secs));
  }
  return buf;
}

std::string pathToDisplayName(const std::string& path) {
  const auto slashPos = path.rfind('/');
  std::string filename = (slashPos != std::string::npos) ? path.substr(slashPos + 1) : path;
  const auto dotPos = filename.rfind('.');
  if (dotPos == std::string::npos || dotPos == 0) {
    return filename;
  }
  return filename.substr(0, dotPos);
}

}  // namespace reading_stats
