#pragma once
#include <I18n.h>

#include <functional>
#include <vector>

#include "SettingInfo.h"
#include "activities/MenuListActivity.h"

// Displays a flat list of SettingInfo items launched from a SettingsActivity submenu entry.
// Supports subcategory separators (withSubcategory) exactly as the parent settings tabs do.
class SettingsSubmenuActivity final : public MenuListActivity {
  StrId titleId;
  std::function<std::string(const SettingInfo&)> itemValueStringOverride;
  bool needsHalfRefresh = false;

  // MenuListActivity overrides
  void onEnter() override;
  void toggleCurrentItem() override;
  void onActionSelected(int index) override;
  void onSettingToggled(int index) override;
  std::string getItemValueString(int index) const override;

 public:
  explicit SettingsSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                                   std::vector<SettingInfo> items,
                                   std::function<std::string(const SettingInfo&)> itemValueStringOverride = {})
      : MenuListActivity("SettingsSubmenu", renderer, mappedInput),
        titleId(titleId),
        itemValueStringOverride(std::move(itemValueStringOverride)) {
    menuItems = std::move(items);
  }

  void render(RenderLock&&) override;
};
