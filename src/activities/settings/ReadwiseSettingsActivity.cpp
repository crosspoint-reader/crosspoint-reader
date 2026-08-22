#include "ReadwiseSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadwiseSyncActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum Row { ROW_API_KEY = 0, ROW_TAG = 1, ROW_SYNC_NOW = 2 };
}  // namespace

ReadwiseSettingsActivity::ReadwiseSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadwiseSettings", renderer, mappedInput) {
  // Labels never change (unlike the values, which track live settings state),
  // so they're set once here rather than every buildScreen() call.
  const StrId menuNames[MENU_ITEMS] = {StrId::STR_READWISE_API_KEY, StrId::STR_READWISE_TAG, StrId::STR_SYNC_NOW};
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int ReadwiseSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* ReadwiseSettingsActivity::headerTitle() const { return tr(STR_READWISE); }

void ReadwiseSettingsActivity::activateIndex(const int index) {
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  app.clearTapFlash();
  if (index == ROW_API_KEY) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READWISE_API_KEY),
                                                                   SETTINGS.readwiseApiKey,
                                                                   sizeof(SETTINGS.readwiseApiKey) - 1, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               snprintf(SETTINGS.readwiseApiKey, sizeof(SETTINGS.readwiseApiKey), "%s",
                                        kb.text.c_str());
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (index == ROW_TAG) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READWISE_TAG),
                                                                   SETTINGS.readwiseTag,
                                                                   sizeof(SETTINGS.readwiseTag) - 1, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               snprintf(SETTINGS.readwiseTag, sizeof(SETTINGS.readwiseTag), "%s", kb.text.c_str());
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (index == ROW_SYNC_NOW) {
    startActivityForResult(std::make_unique<ReadwiseSyncActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  }
}

void ReadwiseSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rowValues_[ROW_API_KEY] = SETTINGS.readwiseApiKey[0] != '\0' ? "******" : tr(STR_NOT_SET);
  rowValues_[ROW_TAG] = SETTINGS.readwiseTag[0] != '\0' ? SETTINGS.readwiseTag : "";
  rowValues_[ROW_SYNC_NOW] = "";

  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}
