#include "DictionarySettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "../reader/LookupHistoryActivity.h"
#include "MappedInputManager.h"
#include "fontIds.h"

void DictionarySettingsActivity::onEnter() {
  Activity::onEnter();
  dictManager.scan();
  selectedIndex = 0;  // Starts on "Lookup History" item
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

  const int dictCount = dictManager.getDictionaryCount();
  const int totalItems = dictListOffset() + dictCount;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == HISTORY_ITEM_INDEX) {
      // Flush enabled state so LookupHistoryActivity sees current toggles
      dictManager.saveEnabledState();
      startActivityForResult(std::make_unique<LookupHistoryActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
    } else {
      toggleSelected();
    }
    return;
  }

  buttonNavigator.onNextRelease([this, dictCount, totalItems] {
    if (totalItems == 0) return;
    // Navigate forward, skipping corrupt dictionaries
    for (int i = 0; i < totalItems; ++i) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      // History item is always selectable
      if (selectedIndex == HISTORY_ITEM_INDEX) break;
      // Check if this dictionary is corrupt
      const int dictIdx = selectedIndex - dictListOffset();
      if (dictIdx >= 0 && dictIdx < dictCount && !dictManager.getDictionary(dictIdx).corrupt) break;
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, dictCount, totalItems] {
    if (totalItems == 0) return;
    for (int i = 0; i < totalItems; ++i) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      if (selectedIndex == HISTORY_ITEM_INDEX) break;
      const int dictIdx = selectedIndex - dictListOffset();
      if (dictIdx >= 0 && dictIdx < dictCount && !dictManager.getDictionary(dictIdx).corrupt) break;
    }
    requestUpdate();
  });
}

void DictionarySettingsActivity::toggleSelected() {
  const int dictIdx = selectedIndex - dictListOffset();
  const int count = dictManager.getDictionaryCount();
  if (dictIdx < 0 || dictIdx >= count) return;

  const auto& dict = dictManager.getDictionary(dictIdx);
  if (dict.corrupt) return;

  dictManager.setEnabled(dictIdx, !dict.enabled);
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
  const int dictCount = dictManager.getDictionaryCount();
  const int totalItems = dictListOffset() + dictCount;

  if (dictCount == 0) {
    // No dictionaries found — show "Lookup History" as the only list item, plus instructions below
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, 1, selectedIndex,
        [](int) -> std::string { return std::string(tr(STR_LOOKUP_HISTORY)); }, nullptr, nullptr,
        [](int) -> std::string { return std::string(">"); }, false);
    // Instructions below the single-item list
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int instructionY = contentTop + metrics.listRowHeight + lineHeight;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, instructionY, tr(STR_NO_DICTIONARIES_FOUND), true);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, instructionY + lineHeight * 2,
                      tr(STR_DICTIONARY_INSTRUCTIONS), true);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
        [this, dictCount](int index) -> std::string {
          if (index == HISTORY_ITEM_INDEX) {
            return std::string(tr(STR_LOOKUP_HISTORY));
          }
          const int dictIdx = index - dictListOffset();
          if (dictIdx < 0 || dictIdx >= dictCount) return "";
          const auto& dict = dictManager.getDictionary(dictIdx);
          char label[96];
          if (dict.corrupt) {
            snprintf(label, sizeof(label), "%s %s", dict.displayName, tr(STR_DICTIONARY_INVALID));
          } else if (dict.readOnly) {
            snprintf(label, sizeof(label), "%s %s", dict.displayName, tr(STR_DICTIONARY_READ_ONLY));
          } else {
            snprintf(label, sizeof(label), "%s", dict.displayName);
          }
          return std::string(label);
        },
        nullptr, nullptr,
        [this, dictCount](int index) -> std::string {
          if (index == HISTORY_ITEM_INDEX) {
            return std::string(">");  // Visual indicator that it's a navigable item
          }
          const int dictIdx = index - dictListOffset();
          if (dictIdx < 0 || dictIdx >= dictCount) return "";
          const auto& dict = dictManager.getDictionary(dictIdx);
          if (dict.corrupt) return std::string("");
          return std::string(dict.enabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        },
        true);
  }

  // Button hints — Confirm label changes based on selected item
  const char* confirmLabel = "";
  if (totalItems > 0) {
    confirmLabel = (selectedIndex == HISTORY_ITEM_INDEX) ? tr(STR_SELECT) : tr(STR_TOGGLE);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
