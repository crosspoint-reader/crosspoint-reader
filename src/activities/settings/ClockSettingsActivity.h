#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Clock configuration under System settings: timezone, 12/24-hour format,
// home-header display, and manual NTP sync. Only reachable when
// halClock.isAvailable() — SettingsActivity gates the entry.
class ClockSettingsActivity final : public UiListActivity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 5;

  void onEnter() override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::string rowValues_[ITEM_COUNT];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
