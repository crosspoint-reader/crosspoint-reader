#include "DictionarySuggestionsActivity.h"

#include <GfxRenderer.h>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"

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
  const int leftPadding = contentX + metrics.contentSidePadding;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Header
  GUI.drawHeader(
      renderer,
      Rect{contentX, hintGutterHeight + metrics.topPadding, pageWidth - hintGutterWidth, metrics.headerHeight},
      "Did you mean?");

  // Subtitle: the original word (manual, below header)
  const int subtitleY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + 5;
  std::string subtitle = "\"" + originalWord + "\" not found";
  renderer.drawText(SMALL_FONT_ID, leftPadding, subtitleY, subtitle.c_str());

  // Suggestion list
  const int listTop = subtitleY + 25;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{contentX, listTop, pageWidth - hintGutterWidth, listHeight}, suggestions.size(), selectedIndex,
      [this](int index) { return suggestions[index]; }, nullptr, nullptr, nullptr);

  // Button hints
  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
