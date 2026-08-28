#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/UiTabListActivity.h"
#include "activities/settings/SettingsTypes.h"
#include "components/OptionPopup.h"

class SettingsActivity final : public UiTabListActivity {
  int selectedCategoryIndex = 0;  // Currently selected category
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  // Extra tabs appended after the built-in 4, from the active
  // SettingsExtensionProvider (see SettingsExtension.h). Empty when no
  // provider is registered, which is the default.
  std::vector<SettingsExtensionCategory> extraCategories;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue) for *currentSettings, rebuilt only when
  // the active category or a category's setting list changes
  // (rebuildRowItems(), called from selectCategory()/rebuildSettingsLists())
  // — not on every repaint. rowValues_ holds the live per-row value text,
  // refreshed every buildScreen() call by assigning into the existing
  // strings (no vector growth).
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  // --- UiTabListActivity contract ---
  int listCount() const override { return settingsCount; }
  int tabCount() const override { return categoryCount + static_cast<int>(extraCategories.size()); }
  int activeTab() const override { return selectedCategoryIndex; }
  const char* tabLabel(int index) const override {
    return index < categoryCount ? I18N.get(categoryNames[index])
                                 : extraCategories[index - categoryCount].label.c_str();
  }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  bool handleCustomInput() override;

  static std::string settingValueText(const SettingInfo& setting);
  void selectCategory(int categoryIndex);
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
};
