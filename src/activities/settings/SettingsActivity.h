#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/UiTabListActivity.h"
#include "activities/settings/SettingsCategoryGridLayout.h"
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

  // The tab band as it stands right now: the built-in categories followed by
  // however many the active provider returned on the last rebuild. Recomputed
  // per call rather than cached, because the provider's count can change
  // between rebuilds and a cached copy is what goes stale.
  SettingsTabRange tabRange() const { return {categoryCount, static_cast<int>(extraCategories.size())}; }

  // --- Category landing screen (touch boards only) --------------------------
  // Six categories across one tab band truncate every label ("Disp... Rea...
  // Con..."), and the pills are poor tap targets. On touch hardware the band
  // is therefore replaced by a landing screen of icon + full-name cards, one
  // per category, and the row list becomes a drill-down under it. Both screens
  // read the SAME tabRange()/tabLabel()/currentSettings model the tab band
  // reads, so no setting is declared twice.
  //
  // Fixed at onEnter() from mappedInput.hasTouch(): false on X3/X4, which keeps
  // the tab band and every button path below byte-for-byte as they were.
  bool categoryLanding = false;
  // Which of the two screens is showing while categoryLanding is on.
  bool onCategoryLanding = false;
  // Card focus on the landing screen, for touch boards that also have physical
  // buttons (Paper Mono, Murphy M3). Drawn only once a button has actually
  // moved it: tap-first hardware should not open on an inverted card that
  // reads as a pre-made choice.
  int landingSelected = 0;
  bool landingFocusVisible = false;

  static constexpr freeink::ui::ActionId ACTION_CATEGORY_CARD = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_CATEGORY_BACK = ACTION_TAB_USER + 1;

  void buildCategoryLanding(UiScreen& screen);
  void buildCategoryBackRow(UiScreen& screen);
  SettingsCategoryGridLayout::Metrics landingMetrics(const UiScreen& screen) const;
  // large: the 32px asset (roomy cards) rather than the 24px one.
  freeink::ui::BitmapRef categoryIcon(int index, bool large) const;
  void openCategoryFromLanding(int index);
  void returnToCategoryLanding();
  void moveLandingSelectionTo(int index);
  static void categoryCardTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void categoryBackTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // --- UiTabListActivity contract ---
  // The landing screen has no rows of its own; its cards are their own
  // interaction surface, so the base's list machinery stays inert on it.
  int listCount() const override { return onCategoryLanding ? 0 : settingsCount; }
  int tabCount() const override { return tabRange().count(); }
  int activeTab() const override { return selectedCategoryIndex; }
  const char* tabLabel(int index) const override {
    const auto range = tabRange();
    return range.isExtension(index) ? extraCategories[range.extraIndex(index)].label.c_str()
                                    : I18N.get(categoryNames[index]);
  }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  bool handleCustomInput() override;
  void navigateButtons() override;

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
