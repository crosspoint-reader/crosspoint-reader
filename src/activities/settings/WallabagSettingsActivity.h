#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for Wallabag settings.
 * Shows server URL, client ID, client secret, username, password, and article limit.
 */
class WallabagSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit WallabagSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onBack)
      : ActivityWithSubactivity("WallabagSettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  const std::function<void()> onBack;

  void handleSelection();

  // Article limit preset cycling
  static constexpr uint8_t LIMIT_PRESETS[] = {10, 20, 30, 50, 100, 0};
  static constexpr int LIMIT_PRESET_COUNT = 6;
  void cycleArticleLimit();
  std::string articleLimitLabel() const;
};
