#include "DictionaryHistoryActivity.h"

#include <I18n.h>

#include "components/UITheme.h"
#include "util/DictionaryHistory.h"

namespace fui = freeink::ui;

const char* DictionaryHistoryActivity::headerTitle() const { return tr(STR_DICT_HISTORY); }

void DictionaryHistoryActivity::onEnter() {
  // The base onEnter schedules a paint; hold the lock until the rows are filled.
  RenderLock lock;
  UiListActivity::onEnter();
  dicthistory::load(words);
  rowItems.assign(words.empty() ? 1 : words.size(), fui::ListItem{});
  if (words.empty()) {
    rowItems[0].label = tr(STR_DICT_HISTORY_EMPTY);
    rowItems[0].enabled = false;
    return;
  }
  for (size_t i = 0; i < words.size(); ++i) {
    rowItems[i].label = words[i].c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

void DictionaryHistoryActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(words.size())) return;
  setResult(KeyboardResult{words[index]});
  finish();
}

void DictionaryHistoryActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void DictionaryHistoryActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
