#pragma once

#include <string>

#include "activities/UiListActivity.h"

// Today's reading against the daily goal, all-time totals and the day streak,
// opened from Home.
// Rows are informational; activating one does nothing.
class ReadingStatsActivity final : public UiListActivity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ROW_COUNT = 8;

  void onEnter() override;

 private:
  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;
  void onBackButton() override { onGoHome(HomeMenuItem::READING_STATS); }

  std::string rowValues[ROW_COUNT];
  freeink::ui::ListItem rowItems[ROW_COUNT]{};
};
