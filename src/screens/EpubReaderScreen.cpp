#include "EpubReaderScreen.h"

#include <EpdRenderer.h>
#include <Epub/Page.h>
#include <SD.h>

#include "Battery.h"

constexpr int PAGES_PER_REFRESH = 15;
constexpr unsigned long SKIP_CHAPTER_MS = 700;

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
    Serial.printf("Loaded cache: %d, %d\n", currentSpineIndex, nextPageNumber);
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
  delete section;
  section = nullptr;
  delete epub;
  epub = nullptr;
}

void EpubReaderScreen::handleInput() {
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

  const bool skipChapter = inputManager.getHeldTime() > SKIP_CHAPTER_MS;

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentSpineIndex = nextReleased ? currentSpineIndex + 1 : currentSpineIndex - 1;
    delete section;
    section = nullptr;
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
      delete section;
      section = nullptr;
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
      delete section;
      section = nullptr;
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

  if (currentSpineIndex >= epub->getSpineItemsCount() || currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex);
    Serial.printf("Loading file: %s, index: %d\n", filepath.c_str(), currentSpineIndex);
    section = new Section(epub, currentSpineIndex, renderer);
    if (!section->loadCacheMetadata()) {
      Serial.println("Cache not found, building...");

      {
        const int textWidth = renderer.getTextWidth("Indexing...");
        constexpr int margin = 20;
        const int x = (renderer.getPageWidth() - textWidth - margin * 2) / 2;
        constexpr int y = 50;
        const int w = textWidth + margin * 2;
        const int h = renderer.getLineHeight() + margin * 2;
        renderer.swapBuffers();
        renderer.fillRect(x, y, w, h, 0);
        renderer.drawText(x + margin, y + margin, "Indexing...");
        renderer.drawRect(x + 5, y + 5, w - 10, h - 10);
        renderer.flushDisplay(EInkDisplay::HALF_REFRESH);
        pagesUntilFullRefresh = 0;
      }

      section->setupCacheDir();
      if (!section->persistPageDataToSD()) {
        Serial.println("Failed to persist page data to SD");
        delete section;
        section = nullptr;
        return;
      }
    } else {
      Serial.println("Cache found, skipping build...");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    Serial.println("No pages to render");
    const int width = renderer.getTextWidth("Empty chapter", BOLD);
    renderer.drawText((renderer.getPageWidth() - width) / 2, 300, "Empty chapter", true, BOLD);
    renderStatusBar();
    renderer.flushDisplay();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("Page out of bounds: %d (max %d)\n", section->currentPage, section->pageCount);
    const int width = renderer.getTextWidth("Out of bounds", BOLD);
    renderer.drawText((renderer.getPageWidth() - width) / 2, 300, "Out of bounds", true, BOLD);
    renderStatusBar();
    renderer.flushDisplay();
    return;
  }

  const Page* p = section->loadPageFromSD();
  renderContents(p);
  delete p;

  File f = SD.open((epub->getCachePath() + "/progress.bin").c_str(), FILE_WRITE);
  uint8_t data[4];
  data[0] = currentSpineIndex & 0xFF;
  data[1] = (currentSpineIndex >> 8) & 0xFF;
  data[2] = section->currentPage & 0xFF;
  data[3] = (section->currentPage >> 8) & 0xFF;
  f.write(data, 4);
  f.close();
}

void EpubReaderScreen::renderContents(const Page* p) {
  p->render(renderer);
  renderStatusBar();
  if (pagesUntilFullRefresh <= 1) {
    renderer.flushDisplay(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = PAGES_PER_REFRESH;
  } else {
    renderer.flushDisplay();
    pagesUntilFullRefresh--;
  }

  // grayscale rendering
  {
    renderer.clearScreen(0x00);
    renderer.setFontRendererMode(GRAYSCALE_LSB);
    p->render(renderer);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setFontRendererMode(GRAYSCALE_MSB);
    p->render(renderer);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setFontRendererMode(BW);
  }
}

void EpubReaderScreen::renderStatusBar() const {
  const auto pageWidth = renderer.getPageWidth();

  const std::string progress = std::to_string(section->currentPage + 1) + " / " + std::to_string(section->pageCount);
  const auto progressTextWidth = renderer.getSmallTextWidth(progress.c_str());
  renderer.drawSmallText(pageWidth - progressTextWidth, 765, progress.c_str());

  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = std::to_string(percentage) + "%";
  const auto percentageTextWidth = renderer.getSmallTextWidth(percentageText.c_str());
  renderer.drawSmallText(20, 765, percentageText.c_str());

  // 1 column on left, 2 columns on right, 5 columns of battery body
  constexpr int batteryWidth = 15;
  constexpr int batteryHeight = 10;
  constexpr int x = 0;
  constexpr int y = 772;

  // Top line
  renderer.drawLine(x, y, x + batteryWidth - 4, y);
  // Bottom line
  renderer.drawLine(x, y + batteryHeight - 1, x + batteryWidth - 4, y + batteryHeight - 1);
  // Left line
  renderer.drawLine(x, y, x, y + batteryHeight - 1);
  // Battery end
  renderer.drawLine(x + batteryWidth - 4, y, x + batteryWidth - 4, y + batteryHeight - 1);
  renderer.drawLine(x + batteryWidth - 3, y + 2, x + batteryWidth - 3, y + batteryHeight - 3);
  renderer.drawLine(x + batteryWidth - 2, y + 2, x + batteryWidth - 2, y + batteryHeight - 3);
  renderer.drawLine(x + batteryWidth - 1, y + 2, x + batteryWidth - 1, y + batteryHeight - 3);

  // The +1 is to round up, so that we always fill at least one pixel
  int filledWidth = percentage * (batteryWidth - 5) / 100 + 1;
  if (filledWidth > batteryWidth - 5) {
    filledWidth = batteryWidth - 5;  // Ensure we don't overflow
  }
  renderer.fillRect(x + 1, y + 1, filledWidth, batteryHeight - 2);

  // Page width minus existing content with 30px padding on each side
  const int leftMargin = 20 + percentageTextWidth + 30;
  const int rightMargin = progressTextWidth + 30;
  const int availableTextWidth = pageWidth - leftMargin - rightMargin;
  const auto tocItem = epub->getTocItem(epub->getTocIndexForSpineIndex(currentSpineIndex));
  auto title = tocItem.title;
  int titleWidth = renderer.getSmallTextWidth(title.c_str());
  while (titleWidth > availableTextWidth) {
    title = title.substr(0, title.length() - 8) + "...";
    titleWidth = renderer.getSmallTextWidth(title.c_str());
  }

  renderer.drawSmallText(leftMargin + (availableTextWidth - titleWidth) / 2, 765, title.c_str());
}
