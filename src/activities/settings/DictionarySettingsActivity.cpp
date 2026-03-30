#include "DictionarySettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void DictionarySettingsActivity::onEnter() {
  Activity::onEnter();
  dictManager.scan();
  selectedIndex = 0;
  requestUpdate();
}

void DictionarySettingsActivity::onExit() {
  dictManager.saveEnabledState();
  Activity::onExit();
}

void DictionarySettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleSelected();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    const int count = dictManager.getDictionaryCount();
    if (count == 0) return;
    // Skip corrupt (unselectable) dictionaries when navigating forward
    for (int i = 0; i < count; ++i) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
      if (!dictManager.getDictionary(selectedIndex).corrupt) break;
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    const int count = dictManager.getDictionaryCount();
    if (count == 0) return;
    // Skip corrupt (unselectable) dictionaries when navigating backward
    for (int i = 0; i < count; ++i) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
      if (!dictManager.getDictionary(selectedIndex).corrupt) break;
    }
    requestUpdate();
  });
}

void DictionarySettingsActivity::toggleSelected() {
  const int count = dictManager.getDictionaryCount();
  if (count == 0 || selectedIndex < 0 || selectedIndex >= count) return;

  const auto& dict = dictManager.getDictionary(selectedIndex);
  if (dict.corrupt) return;  // Corrupt dictionaries are unselectable

  dictManager.setEnabled(selectedIndex, !dict.enabled);
  requestUpdate();
}

void DictionarySettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MANAGE_DICTIONARIES));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int count = dictManager.getDictionaryCount();

  if (count == 0) {
    // No dictionaries found - show instructions
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int centerY = contentTop + contentHeight / 2 - lineHeight;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, centerY, tr(STR_NO_DICTIONARIES_FOUND), true);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, centerY + lineHeight * 2,
                      tr(STR_DICTIONARY_INSTRUCTIONS), true);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, selectedIndex,
        [this](int index) {
          const auto& dict = dictManager.getDictionary(index);
          char label[96];
          if (dict.corrupt) {
            snprintf(label, sizeof(label), "%s %s", dict.displayName, tr(STR_DICTIONARY_INVALID));
          } else if (dict.readOnly) {
            snprintf(label, sizeof(label), "%s %s", dict.displayName, tr(STR_DICTIONARY_READ_ONLY));
          } else {
            snprintf(label, sizeof(label), "%s", dict.displayName);
          }
          return std::string(label);  // std::string required by BaseTheme::drawList API
        },
        nullptr, nullptr,
        [this](int index) {
          const auto& dict = dictManager.getDictionary(index);
          if (dict.corrupt) return std::string("");
          return std::string(dict.enabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));  // drawList API
        },
        true);
  }

  // Button hints
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), count > 0 ? tr(STR_TOGGLE) : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
