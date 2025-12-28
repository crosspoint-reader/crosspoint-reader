#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <InputManager.h>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "config.h"

namespace {
constexpr int pagesPerRefresh = 15;
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr float lineCompression = 0.95f;
constexpr int horizontalPadding = 5;
constexpr int statusBarMargin = 19;
}  // namespace

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();

  File f;
  if (FsHelpers::openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    }
    f.close();
  }

  // Save current epub as last opened epub
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&EpubReaderActivity::taskTrampoline, "EpubReaderActivityTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

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

void EpubReaderActivity::loop() {
  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Enter chapter selection activity
  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    // Don't start activity transition while rendering
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new EpubReaderChapterSelectionActivity(
        this->renderer, this->inputManager, epub, currentSpineIndex,
        [this] {
          exitActivity();
          updateRequired = true;
        },
        [this](const int newSpineIndex) {
          if (currentSpineIndex != newSpineIndex) {
            currentSpineIndex = newSpineIndex;
            nextPageNumber = 0;
            section.reset();
          }
          exitActivity();
          updateRequired = true;
        }));
    xSemaphoreGive(renderingMutex);
  }

  // Long press BACK (1s+) goes directly to home
  if (inputManager.isPressed(InputManager::BTN_BACK) && inputManager.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  // Short press BACK goes to file selection
  if (inputManager.wasReleased(InputManager::BTN_BACK) && inputManager.getHeldTime() < goHomeMs) {
    onGoBack();
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

  const bool skipChapter = inputManager.getHeldTime() > skipChapterMs;

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

void EpubReaderActivity::displayTaskLoop() {
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
void EpubReaderActivity::renderScreen() {
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

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginLeft += horizontalPadding;
  orientedMarginRight += horizontalPadding;
  orientedMarginBottom += statusBarMargin;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const auto viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const auto viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

    if (!section->loadCacheMetadata(READER_FONT_ID, lineCompression, SETTINGS.extraParagraphSpacing, viewportWidth,
                                    viewportHeight)) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      // Progress bar dimensions
      constexpr int barWidth = 200;
      constexpr int barHeight = 10;
      constexpr int boxMargin = 20;
      const int textWidth = renderer.getTextWidth(READER_FONT_ID, "Indexing...");
      const int boxWidthWithBar = (barWidth > textWidth ? barWidth : textWidth) + boxMargin * 2;
      const int boxWidthNoBar = textWidth + boxMargin * 2;
      const int boxHeightWithBar = renderer.getLineHeight(READER_FONT_ID) + barHeight + boxMargin * 3;
      const int boxHeightNoBar = renderer.getLineHeight(READER_FONT_ID) + boxMargin * 2;
      const int boxXWithBar = (renderer.getScreenWidth() - boxWidthWithBar) / 2;
      const int boxXNoBar = (renderer.getScreenWidth() - boxWidthNoBar) / 2;
      constexpr int boxY = 50;
      const int barX = boxXWithBar + (boxWidthWithBar - barWidth) / 2;
      const int barY = boxY + renderer.getLineHeight(READER_FONT_ID) + boxMargin * 2;

      // Always show "Indexing..." text first
      {
        renderer.fillRect(boxXNoBar, boxY, boxWidthNoBar, boxHeightNoBar, false);
        renderer.drawText(READER_FONT_ID, boxXNoBar + boxMargin, boxY + boxMargin, "Indexing...");
        renderer.drawRect(boxXNoBar + 5, boxY + 5, boxWidthNoBar - 10, boxHeightNoBar - 10);
        renderer.displayBuffer();
        pagesUntilFullRefresh = 0;
      }

      section->setupCacheDir();

      // Setup callback - only called for chapters >= 50KB, redraws with progress bar
      auto progressSetup = [this, boxXWithBar, boxWidthWithBar, boxHeightWithBar, barX, barY] {
        renderer.fillRect(boxXWithBar, boxY, boxWidthWithBar, boxHeightWithBar, false);
        renderer.drawText(READER_FONT_ID, boxXWithBar + boxMargin, boxY + boxMargin, "Indexing...");
        renderer.drawRect(boxXWithBar + 5, boxY + 5, boxWidthWithBar - 10, boxHeightWithBar - 10);
        renderer.drawRect(barX, barY, barWidth, barHeight);
        renderer.displayBuffer();
      };

      // Progress callback to update progress bar
      auto progressCallback = [this, barX, barY, barWidth, barHeight](int progress) {
        const int fillWidth = (barWidth - 2) * progress / 100;
        renderer.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, true);
        renderer.displayBuffer(EInkDisplay::FAST_REFRESH);
      };

      if (!section->persistPageDataToSD(READER_FONT_ID, lineCompression, SETTINGS.extraParagraphSpacing, viewportWidth,
                                        viewportHeight, progressSetup, progressCallback)) {
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
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(READER_FONT_ID, 300, "Out of bounds", true, BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
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
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);
  }

  File f;
  if (FsHelpers::openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = section->currentPage & 0xFF;
    data[3] = (section->currentPage >> 8) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  page->render(renderer, READER_FONT_ID, orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = pagesPerRefresh;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();

  // grayscale rendering
  // TODO: Only do this if font supports it
  {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, READER_FONT_ID, orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, READER_FONT_ID, orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // restore the bw data
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  // determine visible status bar elements
  const bool showProgress = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;

  // Position status bar near the bottom of the logical screen, regardless of orientation
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 2;
  int percentageTextWidth = 0;
  int progressTextWidth = 0;

  if (showProgress) {
    // Calculate progress in book
    const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
    const uint8_t bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg);

    // Right aligned text for progress counter
    const std::string progress = std::to_string(section->currentPage + 1) + "/" + std::to_string(section->pageCount) +
                                 "  " + std::to_string(bookProgress) + "%";
    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progress.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progress.c_str());
  }

  if (showBattery) {
    // Left aligned battery icon and percentage
    const uint16_t percentage = battery.readPercentage();
    const auto percentageText = std::to_string(percentage) + "%";
    percentageTextWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, 20 + orientedMarginLeft, textY, percentageText.c_str());

    // 1 column on left, 2 columns on right, 5 columns of battery body
    constexpr int batteryWidth = 15;
    constexpr int batteryHeight = 10;
    const int x = orientedMarginLeft;
    const int y = screenHeight - orientedMarginBottom + 5;

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
  }

  if (showChapterTitle) {
    // Centered chatper title text
    // Page width minus existing content with 30px padding on each side
    const int titleMarginLeft = 20 + percentageTextWidth + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;
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
      while (titleWidth > availableTextWidth && title.length() > 11) {
        title.replace(title.length() - 8, 8, "...");
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}
