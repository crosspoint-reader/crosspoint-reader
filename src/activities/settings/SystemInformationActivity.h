#pragma once

#include <optional>

#include "SystemStatus.h"
#include "activities/Activity.h"

class SystemInformationActivity final : public Activity {
 public:
  explicit SystemInformationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SystemInformation", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::optional<SystemStatus> status_;
  bool sdStatusReady_ = false;
};
