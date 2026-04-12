#pragma once
#include <I18n.h>

#include <vector>

#include "SettingInfo.h"
#include "activities/MenuListActivity.h"

// Displays a flat list of SettingInfo items launched from a SettingsActivity submenu entry.
// Supports subcategory separators (withSubcategory) exactly as the parent settings tabs do.
class SettingsSubmenuActivity final : public MenuListActivity {
  StrId titleId;

  // MenuListActivity overrides
  void onActionSelected(int index) override;
  void onSettingToggled(int index) override;

 public:
  explicit SettingsSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                                   std::vector<SettingInfo> items)
      : MenuListActivity("SettingsSubmenu", renderer, mappedInput), titleId(titleId) {
    menuItems = std::move(items);
  }

  void render(RenderLock&&) override;
};
