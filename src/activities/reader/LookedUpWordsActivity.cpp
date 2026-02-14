#include "LookedUpWordsActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "DictionaryDefinitionActivity.h"
#include "DictionarySuggestionsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

void LookedUpWordsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<LookedUpWordsActivity*>(param);
  self->displayTaskLoop();
}

void LookedUpWordsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void LookedUpWordsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  words = LookupHistory::load(cachePath);
  std::reverse(words.begin(), words.end());
  updateRequired = true;
  xTaskCreate(&LookedUpWordsActivity::taskTrampoline, "LookedUpTask", 4096, this, 1, &displayTaskHandle);
}

void LookedUpWordsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void LookedUpWordsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    if (pendingBackFromDef) {
      pendingBackFromDef = false;
      exitActivity();
      updateRequired = true;
    }
    if (pendingExitToReader) {
      pendingExitToReader = false;
      exitActivity();
      onDone();
    }
    return;
  }

  if (words.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onBack();
    }
    return;
  }

  // Delete confirmation mode: wait for confirm (delete) or back (cancel)
  if (deleteConfirmMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (ignoreNextConfirmRelease) {
        // Ignore the release from the initial long press
        ignoreNextConfirmRelease = false;
      } else {
        // Confirm delete
        LookupHistory::removeWord(cachePath, words[pendingDeleteIndex]);
        words.erase(words.begin() + pendingDeleteIndex);
        if (selectedIndex >= static_cast<int>(words.size())) {
          selectedIndex = std::max(0, static_cast<int>(words.size()) - 1);
        }
        deleteConfirmMode = false;
        updateRequired = true;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteConfirmMode = false;
      ignoreNextConfirmRelease = false;
      updateRequired = true;
    }
    return;
  }

  // Detect long press on Confirm to trigger delete
  constexpr unsigned long DELETE_HOLD_MS = 700;
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= DELETE_HOLD_MS) {
    deleteConfirmMode = true;
    ignoreNextConfirmRelease = true;
    pendingDeleteIndex = selectedIndex;
    updateRequired = true;
    return;
  }

  const int totalItems = static_cast<int>(words.size());
  const int pageItems = getPageItems();

  buttonNavigator.onNextRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    updateRequired = true;
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    updateRequired = true;
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    updateRequired = true;
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    updateRequired = true;
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const std::string& headword = words[selectedIndex];

    Rect popupLayout = GUI.drawPopup(renderer, "Looking up...");
    std::string definition = Dictionary::lookup(
        headword, [this, &popupLayout](int percent) { GUI.fillPopupProgress(renderer, popupLayout, percent); });

    if (!definition.empty()) {
      enterNewActivity(new DictionaryDefinitionActivity(
          renderer, mappedInput, headword, definition, readerFontId, [this]() { pendingBackFromDef = true; },
          [this]() { pendingExitToReader = true; }));
      return;
    }

    // Try stem variants
    auto stems = Dictionary::getStemVariants(headword);
    for (const auto& stem : stems) {
      std::string stemDef = Dictionary::lookup(stem);
      if (!stemDef.empty()) {
        enterNewActivity(new DictionaryDefinitionActivity(
            renderer, mappedInput, stem, stemDef, readerFontId, [this]() { pendingBackFromDef = true; },
            [this]() { pendingExitToReader = true; }));
        return;
      }
    }

    // Show similar word suggestions
    auto similar = Dictionary::findSimilar(headword, 6);
    if (!similar.empty()) {
      enterNewActivity(new DictionarySuggestionsActivity(
          renderer, mappedInput, headword, similar, readerFontId, cachePath, [this]() { pendingBackFromDef = true; },
          [this]() { pendingExitToReader = true; }));
      return;
    }

    GUI.drawPopup(renderer, "Not found");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    vTaskDelay(1500 / portTICK_PERIOD_MS);
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
}

int LookedUpWordsActivity::getPageItems() const {
  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  const int contentTop = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, contentHeight / metrics.listRowHeight);
}

void LookedUpWordsActivity::renderScreen() {
  renderer.clearScreen();

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Header
  GUI.drawHeader(
      renderer,
      Rect{contentX, hintGutterHeight + metrics.topPadding, pageWidth - hintGutterWidth, metrics.headerHeight},
      "Lookup History");

  const int contentTop = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (words.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 20, "No words looked up yet");
  } else {
    GUI.drawList(
        renderer, Rect{contentX, contentTop, pageWidth - hintGutterWidth, contentHeight}, words.size(), selectedIndex,
        [this](int index) { return words[index]; }, nullptr, nullptr, nullptr);
  }

  if (deleteConfirmMode && pendingDeleteIndex < static_cast<int>(words.size())) {
    // Draw delete confirmation overlay
    const std::string& word = words[pendingDeleteIndex];
    std::string displayWord = word;
    if (displayWord.size() > 20) {
      displayWord.erase(17);
      displayWord += "...";
    }
    std::string msg = "Delete '" + displayWord + "'?";

    constexpr int margin = 15;
    const int popupY = 200 + hintGutterHeight;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, msg.c_str(), EpdFontFamily::BOLD);
    const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int w = textWidth + margin * 2;
    const int h = textHeight + margin * 2;
    const int x = contentX + (renderer.getScreenWidth() - hintGutterWidth - w) / 2;

    renderer.fillRect(x - 2, popupY - 2, w + 4, h + 4, true);
    renderer.fillRect(x, popupY, w, h, false);

    const int textX = x + (w - textWidth) / 2;
    const int textY = popupY + margin - 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, msg.c_str(), true, EpdFontFamily::BOLD);

    // Button hints for delete mode
    const auto labels = mappedInput.mapLabels("Cancel", "Delete", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // "Hold select to delete" hint above button hints
    if (!words.empty()) {
      const char* deleteHint = "Hold select to delete";
      const int hintWidth = renderer.getTextWidth(SMALL_FONT_ID, deleteHint);
      const int hintX = contentX + (renderer.getScreenWidth() - hintGutterWidth - hintWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, hintX,
                        renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing * 2,
                        deleteHint);
    }

    // Normal button hints
    const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Select", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
