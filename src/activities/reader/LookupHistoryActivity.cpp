#include "LookupHistoryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "../util/ConfirmationActivity.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "fontIds.h"

void LookupHistoryActivity::onEnter() {
  Activity::onEnter();
  history.load();
  dictManager.scan();
  selectedIndex = 0;
  requestUpdate();
}

void LookupHistoryActivity::onExit() {
  history.save();
  Activity::onExit();
}

void LookupHistoryActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int itemCount = history.getCount() > 0 ? history.getCount() + 1 : 0;  // +1 for "Clear All"

  // Long-press fires immediately at threshold (while button is still held)
  if (!longPressConsumed && itemCount > 0 && selectedIndex < history.getCount() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressConsumed = true;
    confirmDeleteEntry(selectedIndex);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease || longPressConsumed) {
      ignoreNextConfirmRelease = false;
      longPressConsumed = false;
      return;
    }
    if (itemCount == 0) return;

    const bool isClearAll = selectedIndex == history.getCount();
    if (isClearAll) {
      confirmClearAll();
    } else {
      lookupWord(selectedIndex);
    }
    return;
  }

  buttonNavigator.onNextRelease([this, itemCount] {
    if (itemCount == 0) return;
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, itemCount] {
    if (itemCount == 0) return;
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void LookupHistoryActivity::lookupWord(int index) {
  const char* word = history.getWord(index);
  if (word[0] == '\0') return;

  auto* results = static_cast<DictResult*>(malloc(sizeof(DictResult) * DictionaryManager::MAX_RESULTS));
  if (!results) {
    LOG_ERR("HIST", "malloc failed for dictionary results");
    return;
  }

  const int resultCount = dictManager.lookup(word, results, DictionaryManager::MAX_RESULTS);

  if (resultCount > 0) {
    // Bump word to top of history
    history.addWord(word);
    history.save();

    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, word, results, resultCount),
        [this](const ActivityResult&) {
          ignoreNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          requestUpdate();
        });
  } else {
    free(results);
    // Show "no definition found" popup
    GUI.drawPopup(renderer, tr(STR_NO_DEFINITION_FOUND));
    renderer.displayBuffer();
    delay(1500);
    requestUpdate();
  }
}

void LookupHistoryActivity::confirmDeleteEntry(int index) {
  const char* word = history.getWord(index);
  startActivityForResult(std::make_unique<ConfirmationActivity>(
                             renderer, mappedInput, std::string(tr(STR_CONFIRM_DELETE_ENTRY)), std::string(word)),
                         [this, index](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             history.removeWord(index);
                             history.save();
                             // Clamp selectedIndex if it now exceeds list bounds
                             const int itemCount = history.getCount() > 0 ? history.getCount() + 1 : 0;
                             if (itemCount == 0) {
                               selectedIndex = 0;
                             } else if (selectedIndex >= itemCount) {
                               selectedIndex = itemCount - 1;
                             }
                           }
                           ignoreNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
                           requestUpdate();
                         });
}

void LookupHistoryActivity::confirmClearAll() {
  startActivityForResult(std::make_unique<ConfirmationActivity>(
                             renderer, mappedInput, std::string(tr(STR_CONFIRM_CLEAR_HISTORY)), std::string("")),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             history.clear();
                             history.save();
                             selectedIndex = 0;
                           }
                           ignoreNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
                           requestUpdate();
                         });
}

void LookupHistoryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LOOKUP_HISTORY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int historyCount = history.getCount();

  if (historyCount == 0) {
    // Empty state
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int centerY = contentTop + contentHeight / 2 - lineHeight / 2;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, centerY, tr(STR_NO_LOOKUP_HISTORY), true);
  } else {
    const int itemCount = historyCount + 1;  // +1 for "Clear All"

    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
                 [this, historyCount](int index) -> std::string {
                   if (index < historyCount) {
                     return std::string(history.getWord(index));
                   }
                   return std::string(tr(STR_CLEAR_ALL_HISTORY));
                 });
  }

  // Button hints
  const bool hasItems = historyCount > 0;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), hasItems ? tr(STR_SELECT) : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
