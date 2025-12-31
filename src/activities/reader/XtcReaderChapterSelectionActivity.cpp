#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - startY;
  int items = availableHeight / lineHeight;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();
  isFirstRender = true;

  if (!xtc) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = findChapterIndexForPage(currentPage);

  updateRequired = true;
  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void XtcReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& chapters = xtc->getChapters();
    if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
      onSelectPage(chapters[selectorIndex].startPage);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    const int total = static_cast<int>(xtc->getChapters().size());
    if (total == 0) {
      return;
    }
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + total) % total;
    } else {
      selectorIndex = (selectorIndex + total - 1) % total;
    }
    updateRequired = true;
  } else if (nextReleased) {
    const int total = static_cast<int>(xtc->getChapters().size());
    if (total == 0) {
      return;
    }
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % total;
    } else {
      selectorIndex = (selectorIndex + 1) % total;
    }
    updateRequired = true;
  }
}

void XtcReaderChapterSelectionActivity::displayTaskLoop() {
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

void XtcReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();

  // Layout constants matching other screens
  constexpr int headerY = 16;
  constexpr int separatorY = 42;
  constexpr int listStartY = 54;
  constexpr int rowHeight = 28;
  constexpr int horizontalMargin = 16;

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, headerY, "Chapters", true, EpdFontFamily::BOLD);

  // Subtle separator line under header
  renderer.drawLine(horizontalMargin, separatorY, pageWidth - horizontalMargin, separatorY);

  const auto& chapters = xtc->getChapters();
  if (chapters.empty()) {
    const int emptyY = listStartY + 40;
    renderer.drawCenteredText(UI_10_FONT_ID, emptyY, "No chapters found");
    renderer.displayBuffer();
    return;
  }

  // Draw selection highlight
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, listStartY + (selectorIndex % pageItems) * rowHeight - 2, pageWidth - 1, rowHeight);

  // Draw chapter list
  for (int i = pageStartIndex; i < static_cast<int>(chapters.size()) && i < pageStartIndex + pageItems; i++) {
    const auto& chapter = chapters[i];
    const char* title = chapter.name.empty() ? "Unnamed" : chapter.name.c_str();
    const auto truncatedTitle = renderer.truncatedText(UI_10_FONT_ID, title, pageWidth - horizontalMargin * 2 - 8);
    renderer.drawText(UI_10_FONT_ID, horizontalMargin + 4, listStartY + (i % pageItems) * rowHeight,
                      truncatedTitle.c_str(), i != selectorIndex);
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
