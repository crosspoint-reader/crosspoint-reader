#include "BootRecovery.h"

#include <Arduino.h>
#ifndef SIMULATOR
#include <esp_system.h>
#endif

#include <Logging.h>

namespace {
RTC_NOINIT_ATTR BootRecoveryRecord recoveryRecord;
BootRecoveryDecision decision;

bool isCrashReset() {
#ifdef SIMULATOR
  return false;
#else
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
      return true;
    default:
      return false;
  }
#endif
}
}  // namespace

namespace BootRecovery {
void begin(bool manualSafeBoot) {
  decision = BootRecoveryPolicy::begin(recoveryRecord, isCrashReset(), manualSafeBoot);
  if (decision.active) {
    LOG_INF("BOOT", "Safe boot active (%s, failed stage: %s, skip mask: 0x%02x)",
            decision.manual ? "manual" : "automatic", stageName(decision.failedStage), decision.skipMask);
  }
}

bool active() { return decision.active; }
bool manual() { return decision.manual; }
bool shouldSkip(BootStage stage) { return BootRecoveryPolicy::shouldSkip(decision.skipMask, stage); }
BootStage failedStage() { return decision.failedStage; }

const char* stageName(BootStage stage) {
  switch (stage) {
    case BootStage::Settings:
      return "settings";
    case BootStage::AppState:
      return "app state";
    case BootStage::RecentBooks:
      return "recent books";
    case BootStage::KOReader:
      return "KOReader credentials";
    case BootStage::Opds:
      return "OPDS settings";
    case BootStage::SdFonts:
      return "SD fonts";
    case BootStage::None:
    default:
      return "none";
  }
}

void completeStartup() { BootRecoveryPolicy::clear(recoveryRecord); }

StageGuard::StageGuard(BootStage stage) : stage_(stage) { BootRecoveryPolicy::enterStage(recoveryRecord, stage_); }
StageGuard::~StageGuard() { BootRecoveryPolicy::leaveStage(recoveryRecord, stage_); }
}  // namespace BootRecovery
