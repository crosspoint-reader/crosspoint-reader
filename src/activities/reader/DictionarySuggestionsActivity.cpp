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
  GUI.drawHeader(renderer, UITheme::headerRect(renderer), tr(STR_DICT_DID_YOU_MEAN));
  const Rect content = UITheme::contentRect(renderer);
  GUI.drawList(
      renderer, content, static_cast<int>(suggestions.size()), selectedIndex, [this](int i) { return suggestions[i]; },
      nullptr, nullptr, nullptr, true);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
