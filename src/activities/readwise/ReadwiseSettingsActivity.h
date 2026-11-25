#pragma once

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * Settings -> Readwise: Sync now, access token, which locations to mirror,
 * archive-on-finish, remove-archived, and the download folder.
 */
class ReadwiseSettingsActivity final : public UiListActivity {
 public:
  ReadwiseSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 6;

  void render(RenderLock&&) override;

 private:
  OptionPopup optionPopup;

  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
