#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Time threshold for treating a long press as a page-up/page-down
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

bool EpubReaderChapterSelectionActivity::hasSyncOption() const { return KOREADER_STORE.hasCredentials(); }

int EpubReaderChapterSelectionActivity::getTotalItems() const {
  // Add 2 for sync options (top and bottom) if credentials are configured
  const int syncCount = hasSyncOption() ? 2 : 0;
  return epub->getTocItemsCount() + syncCount;
}

bool EpubReaderChapterSelectionActivity::isSyncItem(int index) const {
  if (!hasSyncOption()) return false;
  // First item and last item are sync options
  return index == 0 || index == getTotalItems() - 1;
}

int EpubReaderChapterSelectionActivity::tocIndexFromItemIndex(int itemIndex) const {
  // Account for the sync option at the top
  const int offset = hasSyncOption() ? 1 : 0;
  return itemIndex - offset;
}

int EpubReaderChapterSelectionActivity::getPageItems() const {
  // Layout constants used in renderScreen
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // In inverted portrait, the button hints are drawn near the logical top.
  // Reserve vertical space so list items do not collide with the hints.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  // Clamp to at least one item to avoid division by zero and empty paging.
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();

  // Account for sync option offset when finding current TOC index
  const int syncOffset = hasSyncOption() ? 1 : 0;
  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }
  selectorIndex += syncOffset;  // Offset for top sync option

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
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderChapterSelectionActivity::launchSyncActivity() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new KOReaderSyncActivity(
      renderer, mappedInput, epub, epubPath, currentSpineIndex, currentPage, totalPagesInSpine,
      [this]() {
        // On cancel
        exitActivity();
        updateRequired = true;
      },
      [this](int newSpineIndex, int newPage) {
        // On sync complete
        exitActivity();
        onSyncPosition(newSpineIndex, newPage);
      }));
  xSemaphoreGive(renderingMutex);
}

void EpubReaderChapterSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Check if sync option is selected (first or last item)
    if (isSyncItem(selectorIndex)) {
      launchSyncActivity();
      return;
    }

    // Get TOC index (account for top sync offset)
    const int tocIndex = tocIndexFromItemIndex(selectorIndex);
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(tocIndex);
    if (newSpineIndex == -1) {
      onGoBack();
    } else {
      onSelectSpineIndex(newSpineIndex);
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

void EpubReaderChapterSelectionActivity::displayTaskLoop() {
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

void EpubReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, "Go to Chapter", EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, "Go to Chapter", true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  // Highlight only the content area, not the hint gutters.
  renderer.fillRect(contentX, 60 + contentY + (selectorIndex % pageItems) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentY + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    if (isSyncItem(itemIndex)) {
      // Sync option uses a fixed label and stays aligned to the content margin.
      renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, ">> Sync Progress", !isSelected);
    } else {
      const int tocIndex = tocIndexFromItemIndex(itemIndex);
      auto item = epub->getTocItem(tocIndex);

      // Indent per TOC level while keeping content within the gutter-safe region.
      const int indentSize = contentX + 20 + (item.level - 1) * 15;
      const std::string chapterName =
          renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), contentWidth - 40 - indentSize);

      renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
    }
  }

  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
