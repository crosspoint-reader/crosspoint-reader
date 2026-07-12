#pragma once
#include <cstdint>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BatteryMonitorActivity final : public Activity {
 public:
  explicit BatteryMonitorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BatteryMonitor", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  unsigned long lastSample = 0;
  static constexpr unsigned long SAMPLE_INTERVAL_MS = 30000;
  static constexpr int HISTORY_SIZE = 60;

  struct Sample { uint16_t percent; };
  Sample history[HISTORY_SIZE] = {};
  int historyHead = 0;
  int historyCount = 0;

  uint16_t currentPercent = 0;

  void takeSample();
};
