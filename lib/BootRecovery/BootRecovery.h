#pragma once

#include "BootRecoveryPolicy.h"

namespace BootRecovery {
void begin(bool manualSafeBoot);
bool active();
bool manual();
bool shouldSkip(BootStage stage);
BootStage failedStage();
const char* stageName(BootStage stage);
void completeStartup();

class StageGuard {
 public:
  explicit StageGuard(BootStage stage);
  ~StageGuard();
  StageGuard(const StageGuard&) = delete;
  StageGuard& operator=(const StageGuard&) = delete;

 private:
  BootStage stage_;
};
}  // namespace BootRecovery
