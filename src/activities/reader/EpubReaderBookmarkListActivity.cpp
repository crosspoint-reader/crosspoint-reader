#include "EpubReaderBookmarkListActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int EpubReaderBookmarkListActivity::getTotalItems() const { return static_cast<int>(bookmarks.size()); }

int EpubReaderBookmarkListActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderBookmarkListActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderBookmarkListActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderBookmarkListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  bookmarks = BookmarkStore::loadBookmarks(bookPath);
  renderingMutex = xSemaphoreCreateMutex();

  if (selectorIndex >= getTotalItems()) {
    selectorIndex = std::max(0, getTotalItems() - 1);
  }

  updateRequired = true;
  xTaskCreate(&EpubReaderBookmarkListActivity::taskTrampoline, "BookmarkListTask", 4096, this, 1, &displayTaskHandle);
}

void EpubReaderBookmarkListActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderBookmarkListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = getTotalItems();

  // Handle empty bookmark list
  if (totalItems == 0 && !confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onGoBack();
    }
    return;
  }

  // Delete confirmation mode
  if (confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      BookmarkStore::deleteBookmark(bookPath, selectorIndex);
      bookmarks = BookmarkStore::loadBookmarks(bookPath);
      if (selectorIndex >= getTotalItems()) {
        selectorIndex = std::max(0, getTotalItems() - 1);
      }
      confirmingDelete = false;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      confirmingDelete = false;
      updateRequired = true;
    }
    return;
  }

  // Normal navigation
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() > SKIP_PAGE_MS) {
      confirmingDelete = true;
      updateRequired = true;
    } else if (selectorIndex >= 0 && selectorIndex < totalItems) {
      const auto& bk = bookmarks[selectorIndex];
      onSelectBookmark(bk.spineIndex, bk.pageIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + 1) % totalItems;
    }
    updateRequired = true;
  }
}

void EpubReaderBookmarkListActivity::displayTaskLoop() {
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

void EpubReaderBookmarkListActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const char* titleText = confirmingDelete ? "Delete bookmark?" : "Bookmarks";
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, titleText, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, titleText, true, EpdFontFamily::BOLD);

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "No bookmarks", true);
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(contentX, 60 + contentY + (selectorIndex % pageItems) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentY + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    const auto& bk = bookmarks[itemIndex];
    char label[64];
    if (resolveChapterTitle && bk.chapterPercent > 0) {
      std::string title = resolveChapterTitle(bk.spineIndex);
      if (title.length() > 20) {
        title.resize(17);
        title += "...";
      }
      snprintf(label, sizeof(label), "%d%% of %s - %d%% of book", bk.chapterPercent, title.c_str(), bk.bookPercent);
    } else {
      snprintf(label, sizeof(label), "%d%% of book", bk.bookPercent);
    }

    const int textX = contentX + 20;
    renderer.drawText(UI_10_FONT_ID, textX, displayY, label, !isSelected);
  }

  if (confirmingDelete) {
    const auto labels = mappedInput.mapLabels("Cancel", "Delete", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels("« Back", "Go to", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
