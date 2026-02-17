#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for KOReader Sync settings.
 * Shows username, password, and authenticate options.
 */
class KOReaderSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::function<void()>& onBack)
      : ActivityWithSubactivity("KOReaderSettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  const std::function<void()> onBack;

  void handleSelection();
};
