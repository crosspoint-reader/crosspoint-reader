#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Settings submenu for BookFusion Sync.
 * Shows "Link Account" and "Unlink Account" options with linked/not-linked status.
 */
class BookFusionSettingsActivity final : public Activity {
 public:
  explicit BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int MENU_ITEMS = 3;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};
