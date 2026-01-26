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

void EpubReaderChapterSelectionActivity::buildFilteredChapterList() {
  filteredSpineIndices.clear();

  for (int i = 0; i < epub->getSpineItemsCount(); i++) {
    // Skip footnote pages
    if (epub->shouldHideFromToc(i)) {
      Serial.printf("[%lu] [CHAP] Hiding footnote page at spine index: %d\n", millis(), i);
      continue;
    }

    // Skip pages without TOC entry (unnamed pages)
    int tocIndex = epub->getTocIndexForSpineIndex(i);
    if (tocIndex == -1) {
      Serial.printf("[%lu] [CHAP] Hiding unnamed page at spine index: %d\n", millis(), i);
      continue;
    }

    filteredSpineIndices.push_back(i);
  }

  Serial.printf("[%lu] [CHAP] Filtered chapters: %d out of %d\n", millis(), filteredSpineIndices.size(),
                epub->getSpineItemsCount());
}

void EpubReaderChapterSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();

  // Build filtered chapter list (excluding footnote pages)
  buildFilteredChapterList();

  // Find the index in filtered list that corresponds to currentSpineIndex
  selectorIndex = 0;
  for (size_t i = 0; i < filteredSpineIndices.size(); i++) {
    if (filteredSpineIndices[i] == currentSpineIndex) {
      selectorIndex = i;
      break;
    }
  }

  // Account for sync option offset when finding current TOC index (if applicable)
  // For simplicity, if we are using the filtered list, we might just put "Sync" at the top of THAT list?
  // But wait, the filtered list is spine indices.
  // The master logic used TOC indices directly.
  // Let's adapt: We will display the filtered list.
  // If sync is enabled, we prepend/append it to the selector range.
  
  if (hasSyncOption()) {
      selectorIndex += 1; // Offset for top sync option
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
  
  // Total items = filtered chapters + sync options
  const int syncCount = hasSyncOption() ? 2 : 0;
  const int totalItems = filteredSpineIndices.size() + syncCount;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Check if sync option is selected
    if (hasSyncOption()) {
        if (selectorIndex == 0 || selectorIndex == totalItems - 1) {
            launchSyncActivity();
            return;
        }
    }

    // It's a chapter. Calculate index in filtered list.
    int filteredIndex = selectorIndex;
    if (hasSyncOption()) filteredIndex -= 1; // Remove top sync offset

    if (filteredIndex >= 0 && filteredIndex < filteredSpineIndices.size()) {
      onSelectSpineIndex(filteredSpineIndices[filteredIndex]);
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
  
  const int syncCount = hasSyncOption() ? 2 : 0;
  const int totalItems = filteredSpineIndices.size() + syncCount;

  if (totalItems == 0) {
    renderer.drawCenteredText(SMALL_FONT_ID, 300, "No chapters available", true);
    renderer.displayBuffer();
    return;
  }

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, epub->getTitle().c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title.c_str(), true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * 30 - 2, pageWidth - 1, 30);

  for (int i = pageStartIndex; i < totalItems && i < pageStartIndex + pageItems; i++) {
    const int displayY = 60 + (i % pageItems) * 30;
    const bool isSelected = (i == selectorIndex); // Use i for comparison

    // Check for sync item
    bool isSync = false;
    if (hasSyncOption()) {
        if (i == 0 || i == totalItems - 1) isSync = true;
    }

    if (isSync) {
        renderer.drawText(UI_10_FONT_ID, 20, displayY, ">> Sync Progress", !isSelected);
    } else {
        // It's a filtered chapter
        int filteredIndex = i;
        if (hasSyncOption()) filteredIndex -= 1;

        const int actualSpineIndex = filteredSpineIndices[filteredIndex];
        const int tocIndex = epub->getTocIndexForSpineIndex(actualSpineIndex);

        if (tocIndex == -1) {
             renderer.drawText(UI_10_FONT_ID, 20, displayY, "Unnamed", !isSelected);
        } else {
            auto item = epub->getTocItem(tocIndex);
            const int indentSize = 20 + (item.level - 1) * 15;
            const std::string chapterName =
                renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), pageWidth - 40 - indentSize);
            renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
        }
    }
  }
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
