#include "RecentBooksActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "fontIds.h"

namespace {
// Time threshold for treating a long press as a page-up/page-down
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int RecentBooksActivity::getPageItems() const {
  // Layout constants used in render
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - startY;
  int items = availableHeight / lineHeight;

  // Ensure we always have at least one item per page to avoid division by zero
  if (items < 1) {
    items = 1;
  }
  return items;
}

void RecentBooksActivity::taskTrampoline(void* param) {
  auto* self = static_cast<RecentBooksActivity*>(param);
  self->displayTaskLoop();
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load book titles from recent books list
  bookTitles.clear();
  bookPaths.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  bookTitles.reserve(books.size());
  bookPaths.reserve(books.size());

  for (const auto& path : books) {
    // Skip if file no longer exists
    if (!SdMan.exists(path.c_str())) {
      continue;
    }

    // Extract filename from path for display
    std::string title = path;
    const size_t lastSlash = title.find_last_of('/');
    if (lastSlash != std::string::npos) {
      title = title.substr(lastSlash + 1);
    }

    const std::string ext5 = title.length() >= 5 ? title.substr(title.length() - 5) : "";
    const std::string ext4 = title.length() >= 4 ? title.substr(title.length() - 4) : "";

    // If epub, try to load the metadata for title
    if (ext5 == ".epub") {
      Epub epub(path, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        title = std::string(epub.getTitle());
      }
    } else if (ext5 == ".xtch") {
      title.resize(title.length() - 5);
    } else if (ext4 == ".xtc") {
      title.resize(title.length() - 4);
    }

    bookTitles.push_back(title);
    bookPaths.push_back(path);
  }

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&RecentBooksActivity::taskTrampoline, "RecentBooksActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void RecentBooksActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  bookTitles.clear();
  bookPaths.clear();
}

void RecentBooksActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int bookCount = static_cast<int>(bookTitles.size());

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (bookCount > 0 && selectorIndex < bookCount) {
      onSelectBook(bookPaths[selectorIndex]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased && bookCount > 0) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + bookCount) % bookCount;
    } else {
      selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
    }
    updateRequired = true;
  } else if (nextReleased && bookCount > 0) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % bookCount;
    } else {
      selectorIndex = (selectorIndex + 1) % bookCount;
    }
    updateRequired = true;
  }
}

void RecentBooksActivity::displayTaskLoop() {
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

void RecentBooksActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int bookCount = static_cast<int>(bookTitles.size());

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Recent Books", true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("« Back", "Open", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (bookCount == 0) {
    renderer.drawText(UI_10_FONT_ID, 20, 60, "No recent books");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * 30 - 2, pageWidth - 1, 30);

  for (int i = pageStartIndex; i < bookCount && i < pageStartIndex + pageItems; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, bookTitles[i].c_str(), pageWidth - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % pageItems) * 30, item.c_str(), i != selectorIndex);
  }

  renderer.displayBuffer();
}
