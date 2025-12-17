#include "HomeScreen.h"

#include <GfxRenderer.h>
#include <SD.h>

#include "config.h"

void HomeScreen::taskTrampoline(void* param) {
  auto* self = static_cast<HomeScreen*>(param);
  self->displayTaskLoop();
}

void HomeScreen::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeScreen::taskTrampoline, "HomeScreenTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeScreen::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void HomeScreen::handleInput() {
  const bool prevPressed =
      inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT);
  const bool nextPressed =
      inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT);

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (selectorIndex == 0) {
      onFileSelectionOpen();
    } else if (selectorIndex == 1) {
      onSettingsOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuItemCount - 1) % menuItemCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuItemCount;
    updateRequired = true;
  }
}

void HomeScreen::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeScreen::render() const {
  renderer.clearScreen();

  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();
  renderer.drawCenteredText(READER_FONT_ID, 10, "CrossPoint Reader", true, BOLD);

  // Draw selection
  renderer.fillRect(0, 60 + selectorIndex * 30 + 2, pageWidth - 1, 30);
  renderer.drawText(UI_FONT_ID, 20, 60, "Read", selectorIndex != 0);
  renderer.drawText(UI_FONT_ID, 20, 90, "Settings", selectorIndex != 1);

  renderer.drawRect(25, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 25 + (105 - renderer.getTextWidth(UI_FONT_ID, "Back")) / 2, pageHeight - 35, "Back");

  renderer.drawRect(130, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 130 + (105 - renderer.getTextWidth(UI_FONT_ID, "Confirm")) / 2, pageHeight - 35,
                    "Confirm");

  renderer.drawRect(245, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 245 + (105 - renderer.getTextWidth(UI_FONT_ID, "Left")) / 2, pageHeight - 35, "Left");

  renderer.drawRect(350, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 350 + (105 - renderer.getTextWidth(UI_FONT_ID, "Right")) / 2, pageHeight - 35, "Right");

  renderer.displayBuffer();
}
