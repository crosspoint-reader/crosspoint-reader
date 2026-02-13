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

  const int count = static_cast<int>(suggestions.size());

  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : count - 1;
    updateRequired = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < count - 1) ? selectedIndex + 1 : 0;
    updateRequired = true;
  }

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
        renderer, mappedInput, selected, definition, readerFontId, orientation, [this]() { pendingBackFromDef = true; },
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

  constexpr int sidePadding = 20;
  constexpr int titleY = 15;
  constexpr int subtitleY = 45;
  constexpr int startY = 80;
  constexpr int lineHeight = 30;

  // Title
  renderer.drawText(UI_12_FONT_ID, sidePadding, titleY, "Did you mean?", true, EpdFontFamily::BOLD);

  // Subtitle: the original word
  std::string subtitle = "\"" + originalWord + "\" not found";
  renderer.drawText(SMALL_FONT_ID, sidePadding, subtitleY, subtitle.c_str());

  // Separator
  renderer.drawLine(sidePadding, 68, renderer.getScreenWidth() - sidePadding, 68);

  // Suggestion list
  for (int i = 0; i < static_cast<int>(suggestions.size()); i++) {
    const int displayY = startY + i * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, displayY - 2, renderer.getScreenWidth() - 1, lineHeight);
    }

    renderer.drawText(UI_10_FONT_ID, sidePadding + 10, displayY, suggestions[i].c_str(), !isSelected);
  }

  // Button hints
  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
