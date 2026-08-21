#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Control center contents: one Show/Hide row per quick-setting tile, in the
// panel's own grid order. The frontlight row is deliberately not listed — it is
// what the panel is for, so turning every tile off leaves the brightness
// control rather than an empty sheet.
class ControlCenterSettingsActivity final : public UiListActivity {
 public:
  explicit ControlCenterSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  // Rows, in the order the control center lays its tiles out.
  static constexpr int ITEM_COUNT = 6;

  int listCount() const override { return ITEM_COUNT; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;

  // Labels are fixed (set once in onEnter); only the Show/Hide value text is
  // refreshed per render, in place.
  std::string rowValues_[ITEM_COUNT];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
