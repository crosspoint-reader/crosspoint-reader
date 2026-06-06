#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "JsonSettingsIO.h"

namespace {
constexpr char READING_STATS_FILE_JSON[] = "/.crosspoint/reading_stats.json";
}  // namespace

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::endSession(uint32_t nowMs) {
  aggregator.endSession(nowMs);
  if (!saveToFile()) {
    LOG_ERR("RSS", "Failed to persist reading stats");
  }
}

bool ReadingStatsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadingStats(*this, READING_STATS_FILE_JSON);
}

bool ReadingStatsStore::loadFromFile() {
  if (Storage.exists(READING_STATS_FILE_JSON)) {
    String json = Storage.readFile(READING_STATS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadReadingStats(*this, json.c_str());
    }
  }
  return false;
}
