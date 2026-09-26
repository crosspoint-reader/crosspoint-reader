#pragma once

#include <string>
#include <vector>

#include "SettingsActivity.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Generic per-button Controls submenu (Home Button, Power Button, Side
// Buttons, ...): renders a subset of the shared settings list with the same
// list/popup interaction as SettingsActivity.
class ControlsSubmenuActivity final : public UiListActivity {
 public:
  ControlsSubmenuActivity(GfxRenderer& renderer, MappedInputManager& input, StrId title, std::vector<SettingInfo> rows);

 private:
  StrId title_;
  std::vector<SettingInfo> rows_;
  std::vector<int> visible_;  // indices into rows_, refreshed each buildScreen
  std::vector<std::string> values_;
  std::vector<freeink::ui::ListItem> items_;
  OptionPopup optionPopup;

  void rebuildVisible();
  int listCount() const override { return static_cast<int>(visible_.size()); }
  const char* headerTitle() const override { return I18N.get(title_); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;
};
