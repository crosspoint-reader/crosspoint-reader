#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Read-only device information: detected hardware (device profile, display
// controller, touch, frontlight, RTC, IMU), firmware version, and live
// diagnostics (free heap). Rows are informational; activating one does
// nothing.
class AboutActivity final : public UiListActivity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 12;

  void onEnter() override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;

  std::string rowValues_[ITEM_COUNT];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
