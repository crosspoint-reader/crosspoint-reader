#include "DictionarySuggestionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/DictionaryActivityUtils.h"

void DictionarySuggestionsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void DictionarySuggestionsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(WordResult{suggestions[selectedIndex]});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
  const bool prevItem = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextItem = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);
  if (prevItem && selectedIndex > 0) {
    selectedIndex--;
    requestUpdate();
  }
  if (nextItem && selectedIndex < static_cast<int>(suggestions.size()) - 1) {
    selectedIndex++;
    requestUpdate();
  }
}

void DictionarySuggestionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_DID_YOU_MEAN));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(suggestions.size()), selectedIndex,
      [this](int i) { return suggestions[i]; }, nullptr, nullptr, nullptr, true);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
