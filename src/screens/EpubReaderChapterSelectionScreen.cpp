#include "EpubReaderChapterSelectionScreen.h"

#include <GfxRenderer.h>
#include <SD.h>

#include "config.h"

constexpr int PAGE_ITEMS = 24;
constexpr int SKIP_PAGE_MS = 700;

void EpubReaderChapterSelectionScreen::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionScreen*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionScreen::onEnter() {
  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = currentSpineIndex;

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderChapterSelectionScreen::taskTrampoline, "EpubReaderChapterSelectionScreenTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderChapterSelectionScreen::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderChapterSelectionScreen::handleInput() {
  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  const bool skipPage = inputManager.getHeldTime() > SKIP_PAGE_MS;

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    onSelectSpineIndex(selectorIndex);
  } else if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex =
          ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + epub->getSpineItemsCount()) % epub->getSpineItemsCount();
    } else {
      selectorIndex = (selectorIndex + epub->getSpineItemsCount() - 1) % epub->getSpineItemsCount();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % epub->getSpineItemsCount();
    } else {
      selectorIndex = (selectorIndex + 1) % epub->getSpineItemsCount();
    }
    updateRequired = true;
  }
}

void EpubReaderChapterSelectionScreen::displayTaskLoop() {
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

void EpubReaderChapterSelectionScreen::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(READER_FONT_ID, 10, "Select Chapter", true, BOLD);

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 + 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < epub->getSpineItemsCount() && i < pageStartIndex + PAGE_ITEMS; i++) {
    const int tocIndex = epub->getTocIndexForSpineIndex(i);
    if (tocIndex == -1) {
      renderer.drawText(UI_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, "Unnamed", i != selectorIndex);
    } else {
      auto item = epub->getTocItem(tocIndex);
      renderer.drawText(UI_FONT_ID, 20 + (item.level - 1) * 15, 60 + (i % PAGE_ITEMS) * 30, item.title.c_str(),
                        i != selectorIndex);
    }
  }

  renderer.displayBuffer();
}
