#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <functional>

namespace ReadingStatsStore {

// stats.bin binary layout:
//   [0]   version      uint8_t  = 1
//   [1-4] totalSeconds uint32_t (little-endian)
//   [5]   progress     uint8_t  (0-100)
static constexpr uint8_t STATS_FILE_VERSION = 1;
static constexpr size_t STATS_FILE_SIZE = 6;

std::string getCachePath(const std::string& bookPath) {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
}

BookReadingStats load(const std::string& bookPath) {
  const std::string path = getCachePath(bookPath) + "/stats.bin";
  FsFile f;
  if (!Storage.openFileForRead("STATS", path, f)) {
    return {};
  }

  uint8_t data[STATS_FILE_SIZE];
  const int read = f.read(data, STATS_FILE_SIZE);
  f.close();

  if (read < static_cast<int>(STATS_FILE_SIZE) || data[0] != STATS_FILE_VERSION) {
    return {};
  }

  BookReadingStats stats;
  stats.totalSecondsRead = static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8) |
                           (static_cast<uint32_t>(data[3]) << 16) | (static_cast<uint32_t>(data[4]) << 24);
  stats.lastProgressPercent = data[5];
  return stats;
}

bool save(const std::string& bookPath, const BookReadingStats& stats) {
  const std::string path = getCachePath(bookPath) + "/stats.bin";
  FsFile f;
  if (!Storage.openFileForWrite("STATS", path, f)) {
    LOG_ERR("STATS", "Could not open stats.bin for write");
    return false;
  }

  uint8_t data[STATS_FILE_SIZE];
  data[0] = STATS_FILE_VERSION;
  data[1] = stats.totalSecondsRead & 0xFF;
  data[2] = (stats.totalSecondsRead >> 8) & 0xFF;
  data[3] = (stats.totalSecondsRead >> 16) & 0xFF;
  data[4] = (stats.totalSecondsRead >> 24) & 0xFF;
  data[5] = stats.lastProgressPercent;

  f.write(data, STATS_FILE_SIZE);
  f.close();
  return true;
}

std::string formatTime(uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  char buf[16];
  if (hours > 0) {
    snprintf(buf, sizeof(buf), "%dh %dm", static_cast<int>(hours), static_cast<int>(minutes));
  } else {
    snprintf(buf, sizeof(buf), "%dm", static_cast<int>(minutes));
  }
  return std::string(buf);
}

uint32_t estimateSecondsLeft(uint32_t secondsRead, uint8_t progressPercent) {
  if (progressPercent == 0 || secondsRead == 0 || progressPercent >= 100) {
    return 0;
  }
  const uint32_t remaining = 100u - progressPercent;
  return (secondsRead * remaining) / progressPercent;
}

}  // namespace ReadingStatsStore
