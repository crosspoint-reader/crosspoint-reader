#pragma once

#include "activities/UiListActivity.h"

class ClockTimezoneActivity final : public UiListActivity {
 public:
  explicit ClockTimezoneActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  static constexpr int ROW_WINDOW = 16;

  freeink::ui::ListItem rowItems[ROW_WINDOW]{};
  int windowStart = -1;
  int windowCount = 0;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void refreshRowWindow(int start);
};

