#pragma once

#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "CrossPointSettings.h"
#include "util/ButtonNavigator.h"

class PowerButtonMenuActivity final : public Activity {
 public:
  explicit PowerButtonMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerButtonMenu", renderer, mappedInput) {}

  static bool isActive() { return s_isActive; }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  static inline bool s_isActive = false;

  struct MenuItem {
    CrossPointSettings::SHORT_PWRBTN action;
    StrId labelId;
  };

  const std::vector<MenuItem> items = {
      {CrossPointSettings::SHORT_PWRBTN::SLEEP, StrId::STR_SLEEP},
      {CrossPointSettings::SHORT_PWRBTN::PAGE_TURN, StrId::STR_PAGE_TURN},
      {CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH, StrId::STR_FORCE_REFRESH},
  };

  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};
