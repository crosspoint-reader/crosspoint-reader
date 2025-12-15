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

void EpubReaderChapterSelectionScreen::rebuildVisibleSpineIndices() {
  visibleSpineIndices.clear();
  if (!epub) {
    return;
  }

  const int spineCount = epub->getSpineItemsCount();
  visibleSpineIndices.reserve(spineCount);
  for (int i = 0; i < spineCount; i++) {
    if (epub->getTocIndexForSpineIndex(i) != -1) {
      visibleSpineIndices.push_back(i);
    }
  }
}

void EpubReaderChapterSelectionScreen::onEnter() {
  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  rebuildVisibleSpineIndices();

  selectorIndex = 0;
  if (!visibleSpineIndices.empty()) {
    for (size_t i = 0; i < visibleSpineIndices.size(); i++) {
      if (visibleSpineIndices[i] == currentSpineIndex) {
        selectorIndex = static_cast<int>(i);
        break;
      }
    }
    if (selectorIndex >= static_cast<int>(visibleSpineIndices.size())) {
      selectorIndex = static_cast<int>(visibleSpineIndices.size()) - 1;
    }
  }

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
  visibleSpineIndices.clear();
}

void EpubReaderChapterSelectionScreen::handleInput() {
  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  const bool skipPage = inputManager.getHeldTime() > SKIP_PAGE_MS;

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (!visibleSpineIndices.empty()) {
      if (selectorIndex >= static_cast<int>(visibleSpineIndices.size())) {
        selectorIndex = static_cast<int>(visibleSpineIndices.size()) - 1;
      }
      onSelectSpineIndex(visibleSpineIndices[selectorIndex]);
    }
    return;
  }

  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoBack();
    return;
  }

  const int chapterCount = static_cast<int>(visibleSpineIndices.size());
  if (chapterCount == 0) {
    return;
  }

  if (selectorIndex >= chapterCount) {
    selectorIndex = chapterCount - 1;
  }

  if (prevReleased) {
    if (skipPage) {
      const int totalPages = (chapterCount + PAGE_ITEMS - 1) / PAGE_ITEMS;
      int currentPage = selectorIndex / PAGE_ITEMS;
      currentPage = (currentPage + totalPages - 1) % totalPages;
      selectorIndex = currentPage * PAGE_ITEMS;
      if (selectorIndex >= chapterCount) {
        selectorIndex = chapterCount - 1;
      }
    } else {
      selectorIndex = (selectorIndex + chapterCount - 1) % chapterCount;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      const int totalPages = (chapterCount + PAGE_ITEMS - 1) / PAGE_ITEMS;
      int currentPage = selectorIndex / PAGE_ITEMS;
      currentPage = (currentPage + 1) % totalPages;
      selectorIndex = currentPage * PAGE_ITEMS;
      if (selectorIndex >= chapterCount) {
        selectorIndex = chapterCount - 1;
      }
    } else {
      selectorIndex = (selectorIndex + 1) % chapterCount;
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

  const int chapterCount = static_cast<int>(visibleSpineIndices.size());
  if (chapterCount == 0) {
    renderer.drawText(UI_FONT_ID, 20, 60, "No chapters available");
    renderer.displayBuffer();
    return;
  }

  if (selectorIndex >= chapterCount) {
    selectorIndex = chapterCount - 1;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 + 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < chapterCount && i < pageStartIndex + PAGE_ITEMS; i++) {
    const int spineIndex = visibleSpineIndices[i];
    const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
    if (tocIndex == -1) {
      continue;  // Filtered chapters should not reach here, but skip defensively.
    }
    auto item = epub->getTocItem(tocIndex);
    renderer.drawText(UI_FONT_ID, 20 + (item.level - 1) * 15, 60 + (i % PAGE_ITEMS) * 30, item.title.c_str(),
                      i != selectorIndex);
  }

  renderer.displayBuffer();
}
