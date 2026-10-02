#include "CrossPointState.h"

#include <algorithm>
#include <cstring>

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

void getLCG(uint32_t packedState, uint32_t packedCount, uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
            uint16_t& indexesTotal) {
  indexesLeft = packedCount >> 16;
  indexesTotal = packedCount & 0xffff;
  seed = packedState >> 16;
  state = packedState & 0xffff;
}

void setLCG(uint32_t& packedState, uint32_t& packedCount, uint32_t seedVal, uint32_t stateVal, uint16_t indexesLeftVal,
            uint16_t indexesTotalVal) {
  packedCount = (static_cast<uint32_t>(indexesLeftVal) << 16) | indexesTotalVal;
  packedState = (seedVal << 16) | (stateVal & 0xffff);
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

void CrossPointState::getSleepLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
                                  uint16_t& indexesTotal) const {
  getLCG(sleepLcgState, sleepLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::getSleepOverlayLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
                                         uint16_t& indexesTotal) const {
  getLCG(sleepOverlayLcgState, sleepOverlayLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::setSleepLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal) {
  setLCG(sleepLcgState, sleepLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::setSleepOverlayLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal) {
  setLCG(sleepOverlayLcgState, sleepOverlayLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["sleepLcgCount"] = sleepLcgCount;
  doc["sleepLcgState"] = sleepLcgState;
  JsonArray recentOverlayArr = doc["recentOverlaySleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentOverlayArr.add(recentOverlaySleepImages[i]);
  doc["recentOverlaySleepPos"] = recentOverlaySleepPos;
  doc["recentOverlaySleepFill"] = recentOverlaySleepFill;
  doc["sleepOverlayLcgCount"] = sleepOverlayLcgCount;
  doc["sleepOverlayLcgState"] = sleepOverlayLcgState;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";

  memset(recentSleepImages, 0, sizeof(recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));

  memset(recentOverlaySleepImages, 0, sizeof(recentOverlaySleepImages));
  JsonArrayConst recentOverlayArr = doc["recentOverlaySleepImages"];
  const int actualOverlayCount = recentOverlayArr.isNull() ? 0
                                                           : std::min(static_cast<int>(recentOverlayArr.size()),
                                                                      static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualOverlayCount; i++) {
    recentOverlaySleepImages[i] = recentOverlayArr[i] | static_cast<uint16_t>(0);
  }
  recentOverlaySleepPos = doc["recentOverlaySleepPos"] | static_cast<uint8_t>(0);
  if (recentOverlaySleepPos >= SLEEP_RECENT_COUNT) {
    recentOverlaySleepPos = actualOverlayCount > 0 ? recentOverlaySleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  }
  recentOverlaySleepFill = doc["recentOverlaySleepFill"] | static_cast<uint8_t>(0);
  recentOverlaySleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentOverlaySleepFill), actualOverlayCount));

  sleepLcgCount = doc["sleepLcgCount"] | static_cast<uint32_t>(0);
  sleepLcgState = doc["sleepLcgState"] | static_cast<uint32_t>(0);

  sleepOverlayLcgCount = doc["sleepOverlayLcgCount"] | static_cast<uint32_t>(0);
  sleepOverlayLcgState = doc["sleepOverlayLcgState"] | static_cast<uint32_t>(0);

  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  showBootScreen = doc["showBootScreen"] | true;
  return true;
}
