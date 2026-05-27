#pragma once

#include "activities/Activity.h"

class HabitTrackerActivity final : public Activity {
 public:
  HabitTrackerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HabitTracker", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
