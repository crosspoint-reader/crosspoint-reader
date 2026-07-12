#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DeviceInfoActivity final : public Activity {
 public:
  explicit DeviceInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeviceInfo", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int scrollOffset = 0;
  static constexpr int LINE_COUNT = 9;  // total info lines
};
