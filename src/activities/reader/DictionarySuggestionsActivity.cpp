#include "DictionarySuggestionsActivity.h"

#include <GfxRenderer.h>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/LookupHistory.h"

void DictionarySuggestionsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<DictionarySuggestionsActivity*>(param);
  self->displayTaskLoop();
}

void DictionarySuggestionsActivity::displayTaskLoop() {
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

void DictionarySuggestionsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&DictionarySuggestionsActivity::taskTrampoline, "DictSugTask", 4096, this, 1, &displayTaskHandle);
}

void DictionarySuggestionsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void DictionarySuggestionsActivity::loop() {
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

  if (suggestions.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onBack();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(suggestions.size()));
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(suggestions.size()));
    updateRequired = true;
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const std::string& selected = suggestions[selectedIndex];
    std::string definition = Dictionary::lookup(selected);

    if (definition.empty()) {
      GUI.drawPopup(renderer, "Not found");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      updateRequired = true;
      return;
    }

    LookupHistory::addWord(cachePath, selected);
    enterNewActivity(new DictionaryDefinitionActivity(
        renderer, mappedInput, selected, definition, readerFontId, [this]() { pendingBackFromDef = true; },
        [this]() { pendingExitToReader = true; }));
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
}

void DictionarySuggestionsActivity::renderScreen() {
  renderer.clearScreen();

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding;
  const int leftPadding = contentX + sidePadding;
  const int rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;
  const int contentWidth = renderer.getScreenWidth() - leftPadding - rightPadding;

  const int titleY = 15 + hintGutterHeight;
  const int subtitleY = 45 + hintGutterHeight;
  const int separatorY = 68 + hintGutterHeight;
  const int startY = 80 + hintGutterHeight;
  constexpr int lineHeight = 30;

  // Title
  renderer.drawText(UI_12_FONT_ID, leftPadding, titleY, "Did you mean?", true, EpdFontFamily::BOLD);

  // Subtitle: the original word
  std::string subtitle = "\"" + originalWord + "\" not found";
  renderer.drawText(SMALL_FONT_ID, leftPadding, subtitleY, subtitle.c_str());

  // Separator
  renderer.drawLine(leftPadding, separatorY, renderer.getScreenWidth() - rightPadding, separatorY);

  // Suggestion list
  for (int i = 0; i < static_cast<int>(suggestions.size()); i++) {
    const int displayY = startY + i * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, displayY - 2, contentWidth + sidePadding * 2, lineHeight);
    }

    renderer.drawText(UI_10_FONT_ID, leftPadding + 10, displayY, suggestions[i].c_str(), !isSelected);
  }

  // Button hints
  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
