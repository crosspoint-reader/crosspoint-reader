#include "HomeActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 3;  // Browse files, File transfer, Settings
  if (hasContinueReading) count++;
  if (hasOpdsUrl) count++;
  return count;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  if (hasContinueReading) {
    // Extract filename from path for display
    lastBookTitle = APP_STATE.openEpubPath;
    const size_t lastSlash = lastBookTitle.find_last_of('/');
    if (lastSlash != std::string::npos) {
      lastBookTitle = lastBookTitle.substr(lastSlash + 1);
    }

    // If epub, try to load the metadata for title/author
    if (StringUtils::checkFileExtension(lastBookTitle, ".epub")) {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        lastBookTitle = std::string(epub.getTitle());
      }
      if (!epub.getAuthor().empty()) {
        lastBookAuthor = std::string(epub.getAuthor());
      }
    } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtch")) {
      lastBookTitle.resize(lastBookTitle.length() - 5);
    } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
      lastBookTitle.resize(lastBookTitle.length() - 4);
    }
  }

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
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

void HomeActivity::loop() {
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    const int continueIdx = hasContinueReading ? idx++ : -1;
    const int browseFilesIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex == continueIdx) {
      onContinueReading();
    } else if (selectorIndex == browseFilesIdx) {
      onReaderOpen();
    } else if (selectorIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (selectorIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (selectorIndex == settingsIdx) {
      onSettingsOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
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

void HomeActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  constexpr int margin = 20;
  constexpr int bottomMargin = 60;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  const int bookWidth = pageWidth / 2;
  const int bookHeight = pageHeight / 2;
  const int bookX = (pageWidth - bookWidth) / 2;
  constexpr int bookY = 30;
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    if (bookSelected) {
      renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
    } else {
      renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
    }

    // Bookmark icon in the top-right corner of the card
    const int bookmarkWidth = bookWidth / 8;
    const int bookmarkHeight = bookHeight / 5;
    const int bookmarkX = bookX + bookWidth - bookmarkWidth - 8;
    constexpr int bookmarkY = bookY + 1;

    // Main bookmark body (solid)
    renderer.fillRect(bookmarkX, bookmarkY, bookmarkWidth, bookmarkHeight, !bookSelected);

    // Carve out an inverted triangle notch at the bottom center to create angled points
    const int notchHeight = bookmarkHeight / 2;  // depth of the notch
    for (int i = 0; i < notchHeight; ++i) {
      const int y = bookmarkY + bookmarkHeight - 1 - i;
      const int xStart = bookmarkX + i;
      const int width = bookmarkWidth - 2 * i;
      if (width <= 0) {
        break;
      }
      // Draw a horizontal strip in the opposite color to "cut" the notch
      renderer.fillRect(xStart, y, width, 1, bookSelected);
    }
  }

  if (hasContinueReading) {
    // Split into words (avoid stringstream to keep this light on the MCU)
    std::vector<std::string> words;
    words.reserve(8);
    size_t pos = 0;
    while (pos < lastBookTitle.size()) {
      while (pos < lastBookTitle.size() && lastBookTitle[pos] == ' ') {
        ++pos;
      }
      if (pos >= lastBookTitle.size()) {
        break;
      }
      const size_t start = pos;
      while (pos < lastBookTitle.size() && lastBookTitle[pos] != ' ') {
        ++pos;
      }
      words.emplace_back(lastBookTitle.substr(start, pos - start));
    }

    std::vector<std::string> lines;
    std::string currentLine;
    // Extra padding inside the card so text doesn't hug the border
    const int maxLineWidth = bookWidth - 40;
    const int spaceWidth = renderer.getSpaceWidth(UI_12_FONT_ID);

    for (auto& i : words) {
      // If we just hit the line limit (3), stop processing words
      if (lines.size() >= 3) {
        // Limit to 3 lines
        // Still have words left, so add ellipsis to last line
        lines.back().append("...");

        while (!lines.back().empty() && renderer.getTextWidth(UI_12_FONT_ID, lines.back().c_str()) > maxLineWidth) {
          lines.back().resize(lines.back().size() - 5);
          lines.back().append("...");
        }
        break;
      }

      int wordWidth = renderer.getTextWidth(UI_12_FONT_ID, i.c_str());
      while (wordWidth > maxLineWidth && i.size() > 5) {
        // Word itself is too long, trim it
        i.resize(i.size() - 5);
        i.append("...");
        wordWidth = renderer.getTextWidth(UI_12_FONT_ID, i.c_str());
      }

      int newLineWidth = renderer.getTextWidth(UI_12_FONT_ID, currentLine.c_str());
      if (newLineWidth > 0) {
        newLineWidth += spaceWidth;
      }
      newLineWidth += wordWidth;

      if (newLineWidth > maxLineWidth && !currentLine.empty()) {
        // New line too long, push old line
        lines.push_back(currentLine);
        currentLine = i;
      } else {
        currentLine.append(" ").append(i);
      }
    }

    // If lower than the line limit, push remaining words
    if (!currentLine.empty() && lines.size() < 3) {
      lines.push_back(currentLine);
    }

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!lastBookAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      std::string trimmedAuthor = lastBookAuthor;
      // Trim author if too long
      while (renderer.getTextWidth(UI_10_FONT_ID, trimmedAuthor.c_str()) > maxLineWidth && !trimmedAuthor.empty()) {
        trimmedAuthor.resize(trimmedAuthor.size() - 5);
        trimmedAuthor.append("...");
      }
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, trimmedAuthor.c_str(), !bookSelected);
    }

    renderer.drawCenteredText(UI_10_FONT_ID, bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2,
                              "Continue Reading", !bookSelected);
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book");
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below");
  }

  // --- Bottom menu tiles ---
  // Build menu items dynamically
  std::vector<const char*> menuItems = {"Browse Files", "File Transfer", "Settings"};
  if (hasOpdsUrl) {
    // Insert Calibre Library after Browse Files
    menuItems.insert(menuItems.begin() + 1, "Calibre Library");
  }

  const int menuTileWidth = pageWidth - 2 * margin;
  constexpr int menuTileHeight = 45;
  constexpr int menuSpacing = 8;
  const int totalMenuHeight =
      static_cast<int>(menuItems.size()) * menuTileHeight + (static_cast<int>(menuItems.size()) - 1) * menuSpacing;

  int menuStartY = bookY + bookHeight + 15;
  // Ensure we don't collide with the bottom button legend
  const int maxMenuStartY = pageHeight - bottomMargin - totalMenuHeight - margin;
  if (menuStartY > maxMenuStartY) {
    menuStartY = maxMenuStartY;
  }

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int overallIndex = static_cast<int>(i) + (hasContinueReading ? 1 : 0);
    constexpr int tileX = margin;
    const int tileY = menuStartY + static_cast<int>(i) * (menuTileHeight + menuSpacing);
    const bool selected = selectorIndex == overallIndex;

    if (selected) {
      renderer.fillRect(tileX, tileY, menuTileWidth, menuTileHeight);
    } else {
      renderer.drawRect(tileX, tileY, menuTileWidth, menuTileHeight);
    }

    const char* label = menuItems[i];
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = tileX + (menuTileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (menuTileHeight - lineHeight) / 2;  // vertically centered assuming y is top of text

    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, !selected);
  }

  const auto labels = mappedInput.mapLabels("", "Confirm", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  ScreenComponents::drawBattery(renderer, 20, pageHeight - 70);

  renderer.displayBuffer();
}
