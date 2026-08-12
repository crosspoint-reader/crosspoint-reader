#pragma once
#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Reader status bar configuration activity
class StatusBarSettingsActivity final : public UiListActivity {
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Must equal ITEM_COUNT in the .cpp (static_assert'd there) — the max
  // possible row count (RTC-equipped devices show all of them).
  static constexpr int MAX_STATUS_BAR_ITEMS = 11;

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  OptionPopup optionPopup;

  // Decided in onEnter() based on halClock.isAvailable() so clock entries are hidden on X4.
  int visibleItemCount = 0;

  int listCount() const override { return visibleItemCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

  std::string rowValueText(int index);

  void handleSelection();

  // Row storage: MAX_STATUS_BAR_ITEMS is a compile-time constant, so
  // fixed-capacity storage avoids any heap allocation for the row list.
  // Labels are set once in onEnter() (visibleItemCount is decided there);
  // buildScreen() only refreshes the live value text (rowValues_) by
  // assigning into the existing strings (no array growth).
  std::string rowValues_[MAX_STATUS_BAR_ITEMS];
  freeink::ui::ListItem rowItems_[MAX_STATUS_BAR_ITEMS]{};
};
