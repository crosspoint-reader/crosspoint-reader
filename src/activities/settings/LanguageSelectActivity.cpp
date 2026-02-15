#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void LanguageSelectActivity::taskTrampoline(void* param) {
  auto* self = static_cast<LanguageSelectActivity*>(param);
  self->displayTaskLoop();
}

void LanguageSelectActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  totalItems = getLanguageCount();
  renderingMutex = xSemaphoreCreateMutex();

  // Set current selection based on current language
  selectedIndex = static_cast<int>(I18N.getLanguage());

  updateRequired = false;  // Don't trigger render immediately to avoid race with parent activity

  xTaskCreate(&LanguageSelectActivity::taskTrampoline, "LanguageSelectTask", 4096, this, 1, &displayTaskHandle);
}

void LanguageSelectActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void LanguageSelectActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + totalItems - 1) % totalItems;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % totalItems;
    updateRequired = true;
  }
}

void LanguageSelectActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Set the selected language (setLanguage internally calls saveSettings)
  I18N.setLanguage(static_cast<Language>(selectedIndex));

  xSemaphoreGive(renderingMutex);

  // Return to previous page
  onBack();
}

void LanguageSelectActivity::displayTaskLoop() {
  // Wait for parent activity's rendering to complete (screen refresh takes ~422ms)
  // Wait 500ms to be safe and avoid race conditions with parent activity
  vTaskDelay(500 / portTICK_PERIOD_MS);
  updateRequired = true;

  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void LanguageSelectActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  constexpr int rowHeight = 30;

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_LANGUAGE), true, EpdFontFamily::BOLD);

  // Current language marker
  const int currentLang = static_cast<int>(I18N.getLanguage());

  // Draw options
  for (int i = 0; i < totalItems; i++) {
    const int itemY = 60 + i * rowHeight;
    const bool isSelected = (i == selectedIndex);
    const bool isCurrent = (i == currentLang);

    // Draw selection highlight
    if (isSelected) {
      renderer.fillRect(0, itemY - 2, pageWidth - 1, rowHeight);
    }

    // Draw language name - get it from i18n system
    const char* langName = I18N.getLanguageName(static_cast<Language>(i));
    renderer.drawText(UI_10_FONT_ID, 20, itemY, langName, !isSelected);

    // Draw current selection marker
    if (isCurrent) {
      const char* marker = tr(STR_ON_MARKER);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, marker);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, itemY, marker, !isSelected);
    }
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
