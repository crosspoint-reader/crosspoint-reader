#pragma once

#include <cstdint>

enum class BootStage : uint8_t {
  None = 0,
  Settings = 1,
  AppState = 2,
  RecentBooks = 3,
  KOReader = 4,
  Opds = 5,
  SdFonts = 6,
};

struct BootRecoveryRecord {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t skipMask = 0;
  uint8_t currentStage = 0;
  uint8_t reserved[3] = {};
  uint32_t checksum = 0;
};

struct BootRecoveryDecision {
  uint16_t skipMask = 0;
  BootStage failedStage = BootStage::None;
  bool active = false;
  bool manual = false;
};

class BootRecoveryPolicy {
 public:
  static constexpr uint32_t MAGIC = 0x43565342;
  static constexpr uint16_t VERSION = 1;

  static BootRecoveryDecision begin(BootRecoveryRecord& record, bool crashReset, bool manualSafeBoot);
  static void enterStage(BootRecoveryRecord& record, BootStage stage);
  static void leaveStage(BootRecoveryRecord& record, BootStage stage);
  static void clear(BootRecoveryRecord& record);
  static bool valid(const BootRecoveryRecord& record);
  static bool shouldSkip(uint16_t skipMask, BootStage stage);
  static uint16_t allStageMask();

 private:
  static uint16_t bitFor(BootStage stage);
  static uint32_t checksum(const BootRecoveryRecord& record);
  static void seal(BootRecoveryRecord& record);
};
