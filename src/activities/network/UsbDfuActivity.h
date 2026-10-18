#pragma once
#include "activities/Activity.h"

class UsbDfuActivity final : public Activity {
 public:
  UsbDfuActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("UsbDfu", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool requiresExclusiveStorageLoop() const override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  bool finished = false;
};
