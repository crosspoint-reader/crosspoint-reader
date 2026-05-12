#pragma once
#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "util/ButtonNavigator.h"

class ReaderOptionsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int settingsCount = 0;
  std::vector<SettingInfo> settings;

  void toggleCurrentSetting();

 public:
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderOptions", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
