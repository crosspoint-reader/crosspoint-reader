#include "EpubReaderScreen.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <SD.h>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "EpubReaderChapterSelectionScreen.h"
#include "config.h"

constexpr int PAGES_PER_REFRESH = 15;
constexpr unsigned long SKIP_CHAPTER_MS = 700;
constexpr float lineCompression = 0.95f;
constexpr int marginTop = 8;
constexpr int marginRight = 10;
constexpr int marginBottom = 22;
constexpr int marginLeft = 10;

void EpubReaderScreen::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderScreen*>(param);
  self->displayTaskLoop();
}

void EpubReaderScreen::onEnter() {
  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();

  // TODO: Move this to a state object
  if (SD.exists((epub->getCachePath() + "/progress.bin").c_str())) {
    File f = SD.open((epub->getCachePath() + "/progress.bin").c_str());
    uint8_t data[4];
    f.read(data, 4);
    currentSpineIndex = data[0] + (data[1] << 8);
    nextPageNumber = data[2] + (data[3] << 8);
    Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    f.close();
  }

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&EpubReaderScreen::taskTrampoline, "EpubReaderScreenTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderScreen::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  section.reset();
  epub.reset();
}

void EpubReaderScreen::handleInput() {
  // Pass input responsibility to sub screen if exists
  if (subScreen) {
    subScreen->handleInput();
    return;
  }

  // Enter chapter selection screen
  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    // Don't start screen transition while rendering
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    subScreen.reset(new EpubReaderChapterSelectionScreen(
        this->renderer, this->inputManager, epub, currentSpineIndex,
        [this] {
          subScreen->onExit();
          subScreen.reset();
          updateRequired = true;
        },
        [this](const int newSpineIndex) {
          if (currentSpineIndex != newSpineIndex) {
            currentSpineIndex = newSpineIndex;
            nextPageNumber = 0;
            section.reset();
          }
          subScreen->onExit();
          subScreen.reset();
          updateRequired = true;
        }));
    subScreen->onEnter();
    xSemaphoreGive(renderingMutex);
  }

  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoHome();
    return;
  }

  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  if (!prevReleased && !nextReleased) {
    return;
  }

  // any botton press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool skipChapter = inputManager.getHeldTime() > SKIP_CHAPTER_MS;

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentSpineIndex = nextReleased ? currentSpineIndex + 1 : currentSpineIndex - 1;
    section.reset();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    updateRequired = true;
    return;
  }

  if (prevReleased) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = UINT16_MAX;
      currentSpineIndex--;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  }
}

void EpubReaderScreen::displayTaskLoop() {
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

// TODO: Failure handling
void EpubReaderScreen::renderScreen() {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(READER_FONT_ID, 300, "End of book", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex);
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    if (!section->loadCacheMetadata(READER_FONT_ID, lineCompression, marginTop, marginRight, marginBottom, marginLeft,
                                    SETTINGS.extraParagraphSpacing)) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      {
        const int textWidth = renderer.getTextWidth(READER_FONT_ID, "Indexing...");
        constexpr int margin = 20;
        const int x = (GfxRenderer::getScreenWidth() - textWidth - margin * 2) / 2;
        constexpr int y = 50;
        const int w = textWidth + margin * 2;
        const int h = renderer.getLineHeight(READER_FONT_ID) + margin * 2;
        renderer.grayscaleRevert();
        uint8_t* fb1 = renderer.getFrameBuffer();
        renderer.swapBuffers();
        memcpy(fb1, renderer.getFrameBuffer(), EInkDisplay::BUFFER_SIZE);
        renderer.fillRect(x, y, w, h, 0);
        renderer.drawText(READER_FONT_ID, x + margin, y + margin, "Indexing...");
        renderer.drawRect(x + 5, y + 5, w - 10, h - 10);
        renderer.displayBuffer();
        pagesUntilFullRefresh = 0;
      }

      section->setupCacheDir();
      if (!section->persistPageDataToSD(READER_FONT_ID, lineCompression, marginTop, marginRight, marginBottom,
                                        marginLeft, SETTINGS.extraParagraphSpacing)) {
        Serial.printf("[%lu] [ERS] Failed to persist page data to SD\n", millis());
        section.reset();
        return;
      }
    } else {
      Serial.printf("[%lu] [ERS] Cache found, skipping build...\n", millis());
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    Serial.printf("[%lu] [ERS] No pages to render\n", millis());
    renderer.drawCenteredText(READER_FONT_ID, 300, "Empty chapter", true, BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(READER_FONT_ID, 300, "Out of bounds", true, BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSD();
    if (!p) {
      Serial.printf("[%lu] [ERS] Failed to load page from SD - clearing section cache\n", millis());
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    const auto start = millis();
    renderContents(std::move(p));
    Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);
  }

  File f = SD.open((epub->getCachePath() + "/progress.bin").c_str(), FILE_WRITE);
  uint8_t data[4];
  data[0] = currentSpineIndex & 0xFF;
  data[1] = (currentSpineIndex >> 8) & 0xFF;
  data[2] = section->currentPage & 0xFF;
  data[3] = (section->currentPage >> 8) & 0xFF;
  f.write(data, 4);
  f.close();
}

void EpubReaderScreen::renderContents(std::unique_ptr<Page> page) {
  page->render(renderer, READER_FONT_ID);
  renderStatusBar();
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = PAGES_PER_REFRESH;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // grayscale rendering
  // TODO: Only do this if font supports it
  {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, READER_FONT_ID);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, READER_FONT_ID);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void EpubReaderScreen::renderStatusBar() const {
  constexpr auto textY = 776;
  // Right aligned text for progress counter
  const std::string progress = std::to_string(section->currentPage + 1) + " / " + std::to_string(section->pageCount);
  const auto progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progress.c_str());
  renderer.drawText(SMALL_FONT_ID, GfxRenderer::getScreenWidth() - marginRight - progressTextWidth, textY,
                    progress.c_str());

  // Left aligned battery icon and percentage
  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = std::to_string(percentage) + "%";
  const auto percentageTextWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  renderer.drawText(SMALL_FONT_ID, 20 + marginLeft, textY, percentageText.c_str());

  // 1 column on left, 2 columns on right, 5 columns of battery body
  constexpr int batteryWidth = 15;
  constexpr int batteryHeight = 10;
  constexpr int x = marginLeft;
  constexpr int y = 783;

  // Top line
  renderer.drawLine(x, y, x + batteryWidth - 4, y);
  // Bottom line
  renderer.drawLine(x, y + batteryHeight - 1, x + batteryWidth - 4, y + batteryHeight - 1);
  // Left line
  renderer.drawLine(x, y, x, y + batteryHeight - 1);
  // Battery end
  renderer.drawLine(x + batteryWidth - 4, y, x + batteryWidth - 4, y + batteryHeight - 1);
  renderer.drawLine(x + batteryWidth - 3, y + 2, x + batteryWidth - 1, y + 2);
  renderer.drawLine(x + batteryWidth - 3, y + batteryHeight - 3, x + batteryWidth - 1, y + batteryHeight - 3);
  renderer.drawLine(x + batteryWidth - 1, y + 2, x + batteryWidth - 1, y + batteryHeight - 3);

  // The +1 is to round up, so that we always fill at least one pixel
  int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
  if (filledWidth > batteryWidth - 5) {
    filledWidth = batteryWidth - 5;  // Ensure we don't overflow
  }
  renderer.fillRect(x + 1, y + 1, filledWidth, batteryHeight - 2);

  // Centered chatper title text
  // Page width minus existing content with 30px padding on each side
  const int titleMarginLeft = 20 + percentageTextWidth + 30 + marginLeft;
  const int titleMarginRight = progressTextWidth + 30 + marginRight;
  const int availableTextWidth = GfxRenderer::getScreenWidth() - titleMarginLeft - titleMarginRight;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

  std::string title;
  int titleWidth;
  if (tocIndex == -1) {
    title = "Unnamed";
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
  } else {
    const auto tocItem = epub->getTocItem(tocIndex);
    title = tocItem.title;
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    while (titleWidth > availableTextWidth) {
      title = title.substr(0, title.length() - 8) + "...";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }
  }

  renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
}
