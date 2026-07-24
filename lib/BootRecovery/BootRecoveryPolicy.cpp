#include "BootRecoveryPolicy.h"

namespace {
constexpr uint8_t FIRST_STAGE = static_cast<uint8_t>(BootStage::Settings);
constexpr uint8_t LAST_STAGE = static_cast<uint8_t>(BootStage::SdFonts);

bool isStage(uint8_t value) { return value >= FIRST_STAGE && value <= LAST_STAGE; }
}  // namespace

uint16_t BootRecoveryPolicy::bitFor(BootStage stage) {
  const uint8_t value = static_cast<uint8_t>(stage);
  return isStage(value) ? static_cast<uint16_t>(1U << (value - FIRST_STAGE)) : 0;
}

uint32_t BootRecoveryPolicy::checksum(const BootRecoveryRecord& record) {
  uint32_t value = 2166136261U;
  const auto mix = [&value](uint32_t part) {
    value ^= part;
    value *= 16777619U;
  };
  mix(record.magic);
  mix(record.version);
  mix(record.skipMask);
  mix(record.currentStage);
  return value;
}

void BootRecoveryPolicy::seal(BootRecoveryRecord& record) {
  record.magic = MAGIC;
  record.version = VERSION;
  record.reserved[0] = 0;
  record.reserved[1] = 0;
  record.reserved[2] = 0;
  record.checksum = checksum(record);
}

bool BootRecoveryPolicy::valid(const BootRecoveryRecord& record) {
  if (record.magic != MAGIC || record.version != VERSION) return false;
  if (record.currentStage != 0 && !isStage(record.currentStage)) return false;
  if ((record.skipMask & ~allStageMask()) != 0) return false;
  return record.checksum == checksum(record);
}

uint16_t BootRecoveryPolicy::allStageMask() {
  uint16_t result = 0;
  for (uint8_t stage = FIRST_STAGE; stage <= LAST_STAGE; ++stage) {
    result |= bitFor(static_cast<BootStage>(stage));
  }
  return result;
}

bool BootRecoveryPolicy::shouldSkip(uint16_t skipMask, BootStage stage) {
  return (skipMask & bitFor(stage)) != 0;
}

BootRecoveryDecision BootRecoveryPolicy::begin(BootRecoveryRecord& record, bool crashReset, bool manualSafeBoot) {
  BootRecoveryDecision decision;
  const bool recordValid = valid(record);

  if (manualSafeBoot) {
    decision.skipMask = allStageMask();
    decision.active = true;
    decision.manual = true;
  } else if (crashReset && recordValid) {
    decision.skipMask = record.skipMask;
    if (isStage(record.currentStage)) {
      decision.failedStage = static_cast<BootStage>(record.currentStage);
      decision.skipMask |= bitFor(decision.failedStage);
    }
    decision.active = decision.skipMask != 0;
  }

  record = {};
  record.skipMask = decision.skipMask;
  seal(record);
  return decision;
}

void BootRecoveryPolicy::enterStage(BootRecoveryRecord& record, BootStage stage) {
  if (!valid(record) || bitFor(stage) == 0) return;
  record.currentStage = static_cast<uint8_t>(stage);
  seal(record);
}

void BootRecoveryPolicy::leaveStage(BootRecoveryRecord& record, BootStage stage) {
  if (!valid(record) || record.currentStage != static_cast<uint8_t>(stage)) return;
  record.currentStage = 0;
  seal(record);
}

void BootRecoveryPolicy::clear(BootRecoveryRecord& record) { record = {}; }
