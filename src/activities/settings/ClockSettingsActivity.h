#pragma once
#include <I18n.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ClockSettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void handleSelection();

 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSettings", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
