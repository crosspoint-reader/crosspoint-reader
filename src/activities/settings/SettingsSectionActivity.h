#pragma once

#include <array>
#include <string>
#include <vector>

#include "SettingsActivity.h"
#include "SettingsSection.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

using SettingsBySection = std::array<std::vector<SettingInfo>, SETTINGS_SECTION_COUNT>;

// Fills every section with its on-device rows: the shared settings list,
// filtered for this board, plus device-only action rows.
void buildSettingsSections(SettingsBySection& out);

// The rows of one SettingsSection; activating a row toggles/cycles it, opens
// its option popup, or launches its sub-screen.
class SettingsSectionActivity final : public UiListActivity {
  const SettingsSection section;
  std::vector<SettingInfo> settings;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue), rebuilt only when the setting list
  // changes; rowValues_ holds the live value text, refreshed every
  // buildScreen() by assigning into the existing strings.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;

  void rebuildSettings();
  void rebuildRowItems();
  void toggleSelectedSetting();
  void openSleepTimeoutPicker();
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  static std::string settingValueText(const SettingInfo& setting);

  int listCount() const override { return static_cast<int>(settings.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

 public:
  SettingsSectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SettingsSection section);
  void onEnter() override;
  void render(RenderLock&& lock) override;
};
