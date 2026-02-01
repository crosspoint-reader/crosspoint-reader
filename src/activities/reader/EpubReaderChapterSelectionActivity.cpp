#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
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
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - lineHeight;

  const int availableHeight = endY - startY;
  int items = availableHeight / lineHeight;

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
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Go to Chapter", true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * 30 - 2, pageWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    if (isSyncItem(itemIndex)) {
      renderer.drawText(UI_10_FONT_ID, 20, displayY, ">> Sync Progress", !isSelected);
    } else {
      const int tocIndex = tocIndexFromItemIndex(itemIndex);
      auto item = epub->getTocItem(tocIndex);

      const int indentSize = 20 + (item.level - 1) * 15;
      const std::string chapterName =
          renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), pageWidth - 40 - indentSize);

      renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
    }
  }

  // Skip button hints in landscape CW mode (they overlap content)
  if (renderer.getOrientation() != GfxRenderer::LandscapeClockwise) {
    const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
