#include "LookedUpWordsActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
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

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(words.size()));
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(words.size()));
    updateRequired = true;
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelectWord(words[selectedIndex]);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
}

void LookedUpWordsActivity::renderScreen() {
  renderer.clearScreen();

  constexpr int sidePadding = 20;
  constexpr int titleY = 15;
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  // Title
  const int titleX =
      (renderer.getScreenWidth() - renderer.getTextWidth(UI_12_FONT_ID, "Lookup History", EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, "Lookup History", true, EpdFontFamily::BOLD);

  if (words.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "No words looked up yet");
  } else {
    const int screenHeight = renderer.getScreenHeight();
    const int pageItems = std::max(1, (screenHeight - startY - 40) / lineHeight);
    const int pageStart = selectedIndex / pageItems * pageItems;

    for (int i = 0; i < pageItems; i++) {
      int idx = pageStart + i;
      if (idx >= static_cast<int>(words.size())) break;

      const int displayY = startY + i * lineHeight;
      const bool isSelected = (idx == selectedIndex);

      if (isSelected) {
        renderer.fillRect(0, displayY - 2, renderer.getScreenWidth() - 1, lineHeight);
      }

      renderer.drawText(UI_10_FONT_ID, sidePadding, displayY, words[idx].c_str(), !isSelected);
    }
  }

  if (deleteConfirmMode && pendingDeleteIndex < static_cast<int>(words.size())) {
    // Draw delete confirmation overlay
    const std::string& word = words[pendingDeleteIndex];
    std::string displayWord = word;
    if (displayWord.size() > 20) {
      displayWord = displayWord.substr(0, 17) + "...";
    }
    std::string msg = "Delete '" + displayWord + "'?";

    constexpr int margin = 15;
    constexpr int popupY = 200;
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, msg.c_str(), EpdFontFamily::BOLD);
    const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int w = textWidth + margin * 2;
    const int h = textHeight + margin * 2;
    const int x = (renderer.getScreenWidth() - w) / 2;

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
      renderer.drawText(SMALL_FONT_ID, (renderer.getScreenWidth() - hintWidth) / 2,
                        renderer.getScreenHeight() - 70, deleteHint);
    }

    // Normal button hints
    const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
