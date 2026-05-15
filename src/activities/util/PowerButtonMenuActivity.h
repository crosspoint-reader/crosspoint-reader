#pragma once

#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "CrossPointSettings.h"
#include "util/ButtonNavigator.h"

class PowerButtonMenuActivity final : public Activity {
 public:
  enum class MenuAction { SLEEP, REFRESH_SCREEN, SCREENSHOT };

  explicit PowerButtonMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerButtonMenu", renderer, mappedInput) {}

  static bool isActive();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  static inline bool s_isActive = false;

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  const std::vector<MenuItem> items = {
      {MenuAction::SLEEP, StrId::STR_SLEEP},
      {MenuAction::REFRESH_SCREEN, StrId::STR_FORCE_REFRESH},
      {MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON},
  };

  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};
