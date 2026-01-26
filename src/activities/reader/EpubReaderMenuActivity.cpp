#include "EpubReaderMenuActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

constexpr int MENU_ITEMS_COUNT = 2;

void EpubReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderMenuActivity::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubReaderMenuTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderMenuActivity::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderMenuActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelectOption(static_cast<MenuOption>(selectorIndex));
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    selectorIndex = (selectorIndex + MENU_ITEMS_COUNT - 1) % MENU_ITEMS_COUNT;
    updateRequired = true;
  } else if (nextReleased) {
    selectorIndex = (selectorIndex + 1) % MENU_ITEMS_COUNT;
    updateRequired = true;
  }
}

void EpubReaderMenuActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 10, "Menu", true, EpdFontFamily::BOLD);

  const char* menuItems[MENU_ITEMS_COUNT] = {"Go to chapter", "View footnotes"};

  const int startY = 100;
  const int itemHeight = 40;

  for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
    const int y = startY + i * itemHeight;

    // Draw selection indicator
    if (i == selectorIndex) {
      renderer.fillRect(10, y + 2, pageWidth - 20, itemHeight - 4);
      renderer.drawText(UI_12_FONT_ID, 30, y, menuItems[i], false);
    } else {
      renderer.drawText(UI_12_FONT_ID, 30, y, menuItems[i], true);
    }
  }

  renderer.displayBuffer();
}
