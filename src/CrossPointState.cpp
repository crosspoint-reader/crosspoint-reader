#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState CrossPointState::instance;

namespace {

bool isRecentIndex(const uint16_t* recentImages, uint8_t recentPos, uint8_t recentFill, uint16_t idx,
                   uint8_t checkCount) {
  const uint8_t effectiveCount = std::min(checkCount, recentFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot =
        (recentPos + CrossPointState::SLEEP_RECENT_COUNT - 1 - i) % CrossPointState::SLEEP_RECENT_COUNT;
    if (recentImages[slot] == idx) return true;
  }
  return false;
}

void pushRecentIndex(uint16_t* recentImages, uint8_t& recentPos, uint8_t& recentFill, uint16_t idx) {
  recentImages[recentPos] = idx;
  recentPos = (recentPos + 1) % CrossPointState::SLEEP_RECENT_COUNT;
  if (recentFill < CrossPointState::SLEEP_RECENT_COUNT) recentFill++;
}

}  // namespace

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  return isRecentIndex(recentSleepImages, recentSleepPos, recentSleepFill, idx, checkCount);
}

bool CrossPointState::isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const {
  return isRecentIndex(recentOverlaySleepImages, recentOverlaySleepPos, recentOverlaySleepFill, idx, checkCount);
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  pushRecentIndex(recentSleepImages, recentSleepPos, recentSleepFill, idx);
}

void CrossPointState::pushRecentOverlaySleep(uint16_t idx) {
  pushRecentIndex(recentOverlaySleepImages, recentOverlaySleepPos, recentOverlaySleepFill, idx);
}

bool CrossPointState::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
}

bool CrossPointState::loadFromFile() {
  // Try JSON first
  if (Storage.exists(STATE_FILE_JSON)) {
    String json = Storage.readFile(STATE_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
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
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }

  for (uint8_t i = 0; i < SLEEP_RECENT_COUNT; i++) {
    recentSleepImages[i] = 0;
    recentOverlaySleepImages[i] = 0;
  }
  recentSleepPos = 0;
  recentSleepFill = 0;
  recentOverlaySleepPos = 0;
  recentOverlaySleepFill = 0;

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  return true;
}
