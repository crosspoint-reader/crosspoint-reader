#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
// Time threshold for treating a long press as a page-up/page-down
constexpr int SKIP_PAGE_MS = 700;
constexpr int headerY = 16;
constexpr int separatorY = 42;
constexpr int listStartY = 54;
constexpr int rowHeight = 28;
constexpr int horizontalMargin = 16;
}  // namespace

int EpubReaderChapterSelectionActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - listStartY;
  int items = availableHeight / rowHeight;

  // Ensure we always have at least one item per page to avoid division by zero
  if (items < 1) {
    items = 1;
  }
  return items;
}

void EpubReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();
  isFirstRender = true;

  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderChapterSelectionActivity::taskTrampoline, "EpubReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
    if (newSpineIndex == -1) {
      onGoBack();
    } else {
      onSelectSpineIndex(newSpineIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex =
          ((selectorIndex / pageItems - 1) * pageItems + epub->getTocItemsCount()) % epub->getTocItemsCount();
    } else {
      selectorIndex = (selectorIndex + epub->getTocItemsCount() - 1) % epub->getTocItemsCount();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % epub->getTocItemsCount();
    } else {
      selectorIndex = (selectorIndex + 1) % epub->getTocItemsCount();
    }
    updateRequired = true;
  }
}

void EpubReaderChapterSelectionActivity::displayTaskLoop() {
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

void EpubReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();

  // Draw header with book title
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, epub->getTitle().c_str(),
                                                   pageWidth - horizontalMargin * 2, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, headerY, title.c_str(), true, EpdFontFamily::BOLD);

  // Subtle separator line under header
  renderer.drawLine(horizontalMargin, separatorY, pageWidth - horizontalMargin, separatorY);

  // Draw selection highlight
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, listStartY + (selectorIndex % pageItems) * rowHeight - 2, pageWidth - 1, rowHeight);

  // Draw chapter list
  for (int tocIndex = pageStartIndex; tocIndex < epub->getTocItemsCount() && tocIndex < pageStartIndex + pageItems;
       tocIndex++) {
    auto item = epub->getTocItem(tocIndex);
    const int indentPx = (item.level - 1) * 12;
    const auto truncatedTitle = renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(),
                                                        pageWidth - horizontalMargin * 2 - 8 - indentPx);
    renderer.drawText(UI_10_FONT_ID, horizontalMargin + 4 + indentPx, listStartY + (tocIndex % pageItems) * rowHeight,
                      truncatedTitle.c_str(), tocIndex != selectorIndex);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("« Back", "Go", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (isFirstRender) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    isFirstRender = false;
  } else {
    renderer.displayBuffer();
  }
}
