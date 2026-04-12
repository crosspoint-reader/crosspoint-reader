#pragma once
#include <I18n.h>

#include <functional>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ClockSettingsActivity final : public Activity {
  enum class Action { USE_CLOCK, CLOCK_FORMAT, TIMEZONE, SYNC_TIME, DETECT_TIMEZONE, NONE };

  struct MenuItem {
    Action action;
    StrId labelId;
    bool isSeparator = false;
    static MenuItem separator(StrId label) { return {Action::NONE, label, true}; }
    [[nodiscard]] std::string getTitle() const;
  };

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  const std::vector<MenuItem> menuItems;

  static std::vector<MenuItem> buildMenuItems();
  void handleSelection();

 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSettings", renderer, mappedInput), menuItems(buildMenuItems()) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
