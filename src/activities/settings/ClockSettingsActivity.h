#pragma once

#include "../MenuListActivity.h"

class ClockSettingsActivity final : public MenuListActivity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : MenuListActivity("ClockSettings", renderer, mappedInput) {
    buildMenuItems();
  }

  void render(RenderLock&&) override;

 private:
  void buildMenuItems();
  void onActionSelected(int index) override;
  void onSettingToggled(int index) override;
};
