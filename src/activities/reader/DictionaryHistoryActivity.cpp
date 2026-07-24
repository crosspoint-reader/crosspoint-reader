#include "DictionaryHistoryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictionaryHistoryStore.h"

namespace {
constexpr unsigned long ERROR_DURATION_MS = 1500;
void indexYield(void*) { vTaskDelay(1); }
}  // namespace

void DictionaryHistoryActivity::onEnter() {
  Activity::onEnter();
  refreshEntries();
  requestUpdate();
}

void DictionaryHistoryActivity::refreshEntries() {
  const auto& source = DICTIONARY_HISTORY.entries();
  entries_.assign(source.begin(), source.end());
  const int itemCount = static_cast<int>(entries_.size()) +
                        (!entries_.empty() && DICTIONARY_HISTORY.isWritable() ? 1 : 0);
  if (itemCount == 0) {
    selected_ = 0;
  } else if (selected_ >= itemCount) {
    selected_ = itemCount - 1;
  }
}

void DictionaryHistoryActivity::lookupSelected() {
  if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
  busy_ = true;
  requestUpdateAndWait();

  bool ok = dictionary_.isOpen() || dictionary_.open(SETTINGS.dictionaryName);
  if (ok && dictionary_.needsIndex()) ok = dictionary_.buildIndex(indexYield);
  std::string definition;
  std::string headword;
  if (ok) ok = dictionary_.lookup(entries_[selected_].c_str(), definition, headword);
  busy_ = false;

  if (!ok) {
    error_ = true;
    errorAt_ = millis();
    requestUpdate();
    return;
  }

  DICTIONARY_HISTORY.record(entries_[selected_]);
  refreshEntries();
  startActivityForResult(
      std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword), std::move(definition)),
      [this](const ActivityResult&) { requestUpdate(); });
}

void DictionaryHistoryActivity::confirmClear() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DICT_HISTORY_CLEAR),
                                             tr(STR_DICT_HISTORY_CLEAR_CONFIRM)),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (!DICTIONARY_HISTORY.clear()) {
            error_ = true;
            errorAt_ = millis();
          }
          refreshEntries();
        }
        requestUpdate();
      });
}

void DictionaryHistoryActivity::loop() {
  if (error_) {
    if (millis() - errorAt_ >= ERROR_DURATION_MS) {
      error_ = false;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int itemCount = static_cast<int>(entries_.size()) +
                        (!entries_.empty() && DICTIONARY_HISTORY.isWritable() ? 1 : 0);
  if (itemCount == 0) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selected_ < static_cast<int>(entries_.size())) {
      lookupSelected();
    } else {
      confirmClear();
    }
    return;
  }
  navigator_.onNextRelease([this, itemCount] {
    selected_ = ButtonNavigator::nextIndex(selected_, itemCount);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this, itemCount] {
    selected_ = ButtonNavigator::previousIndex(selected_, itemCount);
    requestUpdate();
  });
  navigator_.onNextContinuous([this, itemCount] {
    selected_ = ButtonNavigator::nextPageIndex(selected_, itemCount, pageItems_);
    requestUpdate();
  });
  navigator_.onPreviousContinuous([this, itemCount] {
    selected_ = ButtonNavigator::previousPageIndex(selected_, itemCount, pageItems_);
    requestUpdate();
  });
}

void DictionaryHistoryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_DICT_HISTORY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  pageItems_ = std::max(1, contentHeight / 52);
  const int itemCount = static_cast<int>(entries_.size()) +
                        (!entries_.empty() && DICTIONARY_HISTORY.isWritable() ? 1 : 0);
  if (itemCount == 0) {
    const char* message = DICTIONARY_HISTORY.isWritable() ? tr(STR_DICT_HISTORY_EMPTY)
                                                          : tr(STR_DICT_HISTORY_UNAVAILABLE);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + metrics.verticalSpacing, message);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, width, contentHeight}, itemCount, selected_,
        [this](int index) {
          return index < static_cast<int>(entries_.size()) ? entries_[index]
                                                           : std::string(tr(STR_DICT_HISTORY_CLEAR));
        },
        [](int) { return std::string{}; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), itemCount > 0 ? tr(STR_SELECT) : "",
                                            itemCount > 1 ? tr(STR_DIR_UP) : "",
                                            itemCount > 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (busy_) {
    GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));
  } else if (error_) {
    GUI.drawPopup(renderer, tr(STR_DICT_ERROR));
  } else {
    renderer.displayBuffer();
  }
}
