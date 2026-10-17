#pragma once
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "SettingInfo.h"
#include "activities/TabbedUiListActivity.h"

class SettingsActivity final : public TabbedUiListActivity {
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  static constexpr int categoryCount = 4;
  // constexpr, not `static const` with an out-of-line definition: a compile-time table has no
  // reason to occupy .data and be initialized at startup when it can sit in flash.
  static constexpr StrId categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                         StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

  std::vector<SettingInfo::SubmenuData> submenuData;
  bool needsHalfRefresh = false;
  // The tab onEnter() opens on (a categoryNames slot), with its bar focused.
  int initialTab = 0;

  void enterCategory(int categoryIndex);

  static constexpr size_t LIST_WINDOW_CAPACITY = 24;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowLabels;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowValues;
  std::array<freeink::ui::ListItem, LIST_WINDOW_CAPACITY> windowItems;
  uint16_t windowFirst = 0;
  uint16_t windowCount = 0;

  void materializeListWindow();

  // UiListActivity / TabbedUiListActivity overrides
  int listCount() const override { return settingsCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  int tabCount() const override { return categoryCount; }
  const char* tabLabel(int slot) const override { return I18N.get(categoryNames[slot]); }
  [[nodiscard]] int16_t tabBarHeight() const override;
  void onTabSelected(int slot) override { enterCategory(slot); }
  void onBackFromTabs() override;
  void homeFromList() override { onBackFromTabs(); }
  [[nodiscard]] bool isRowSelectable(int index) const override;
  void drawChrome() override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  // Overridden only to ship the frame with this screen's refresh mode; see the definition.
  void render(RenderLock&&) override;

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int tab = 0)
      : TabbedUiListActivity("Settings", renderer, mappedInput), initialTab(tab) {}
  void onEnter() override;
  void onExit() override;
  bool releaseBuriedState() override;
};
