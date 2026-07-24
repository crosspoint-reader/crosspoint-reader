#include "CrossPointState.h"

#include <AtomicJsonFile.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "LegacyStateCodec.h"

namespace {
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

bool CrossPointState::saveToFile() const {
  if (!persistenceWritable) return false;
  std::lock_guard<std::mutex> lock(_mutex);
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
}

bool CrossPointState::loadFromFile() {
  const std::string stateBackup = std::string(STATE_FILE_JSON) + ".bak";
  const std::string stateTemp = std::string(STATE_FILE_JSON) + ".tmp";
  const bool hasJsonState = Storage.exists(STATE_FILE_JSON) || Storage.exists(stateBackup.c_str()) ||
                            Storage.exists(stateTemp.c_str());
  if (hasJsonState) {
    std::string json;
    const AtomicFile::LoadStatus loaded = AtomicJsonFile::load(STATE_FILE_JSON, json);
    if (loaded != AtomicFile::LoadStatus::Primary && loaded != AtomicFile::LoadStatus::Backup &&
        loaded != AtomicFile::LoadStatus::Temp) {
      persistenceWritable = false;
      LOG_ERR("CPS", "Could not recover a valid state.json");
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const bool parsed = JsonSettingsIO::loadState(*this, json.c_str());
    persistenceWritable = parsed;
    return parsed;
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
    persistenceWritable = false;
    return false;
  }

  persistenceWritable = true;
  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }
  LegacyStateCodec::State legacy;
  const LegacyStateCodec::DecodeStatus decoded = LegacyStateCodec::decode(inputFile, legacy);
  const bool closed = inputFile.close();
  if (decoded != LegacyStateCodec::DecodeStatus::Ok || !closed) {
    LOG_ERR("CPS", "Legacy state validation failed (%u)", static_cast<unsigned>(decoded));
    return false;
  }

  std::lock_guard<std::mutex> lock(_mutex);
  openEpubPath = std::move(legacy.openBookPath);
  memset(recentSleepImages, 0, sizeof(recentSleepImages));
  recentSleepPos = 0;
  recentSleepFill = 0;
  if (legacy.lastSleepImage != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy.lastSleepImage));
  readerActivityLoadCount = legacy.readerActivityLoadCount;
  lastSleepFromReader = legacy.lastSleepFromReader;

  return true;
}
