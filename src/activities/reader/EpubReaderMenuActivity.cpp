#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;

  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubMenuTask", 4096, this, 1, &displayTaskHandle);
}

void EpubReaderMenuActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderMenuActivity::displayTaskLoop() {
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

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Use local variables for items we need to check after potential deletion
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + menuItems.size() - 1) % menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // 1. Capture the callback and action locally
    auto actionCallback = onAction;
    auto selectedAction = menuItems[selectedIndex].action;

    // 2. Execute the callback
    actionCallback(selectedAction);

    // 3. CRITICAL: Return immediately. 'this' is likely deleted now.
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;  // Also return here just in case
  }
}

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // Menu Items
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int displayY = startY + (i * lineHeight);
    const bool isSelected = (static_cast<int>(i) == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, displayY, pageWidth - 1, lineHeight, true);
    }

    renderer.drawText(UI_10_FONT_ID, 20, displayY, menuItems[i].label.c_str(), !isSelected);
  }

  // Footer / Hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
