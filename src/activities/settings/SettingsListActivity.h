#pragma once

#include <array>

#include "SettingsSection.h"
#include "activities/UiListActivity.h"

// Settings root: one row per SettingsSection, each opening a
// SettingsActivity with that group's settings.
class SettingsListActivity final : public UiListActivity {
  // Section opened on the first loop pass (End = none), e.g. returning to
  // Network after the Wi-Fi rows' silent restart.
  SettingsSection pendingSection;

  std::array<freeink::ui::ListItem, SETTINGS_SECTION_COUNT> rowItems_{};

  void rebuildRows();
  void openSection(SettingsSection section);

  int listCount() const override { return static_cast<int>(SETTINGS_SECTION_COUNT); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void drawChrome() override;

 public:
  explicit SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                SettingsSection openSection = SettingsSection::End);
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
