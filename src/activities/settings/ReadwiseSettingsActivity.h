#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Submenu for Readwise sync settings: API key, tag, and a Sync Now action.
 */
class ReadwiseSettingsActivity final : public UiListActivity {
 public:
  explicit ReadwiseSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 3;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
