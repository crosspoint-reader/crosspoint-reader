/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <InputManager.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "config.h"

namespace {
constexpr int pagesPerRefresh = 15;
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
}  // namespace

void XtcReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&XtcReaderActivity::taskTrampoline, "XtcReaderActivityTask",
              4096,               // Stack size (smaller than EPUB since no parsing needed)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  xtc.reset();
}

void XtcReaderActivity::loop() {
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

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    updateRequired = true;
    return;
  }

  const bool skipPages = inputManager.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevReleased) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    updateRequired = true;
  } else if (nextReleased) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    updateRequired = true;
  }
}

void XtcReaderActivity::displayTaskLoop() {
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

void XtcReaderActivity::renderScreen() {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_FONT_ID, 300, "End of book", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    Serial.printf("[%lu] [XTR] Failed to allocate page buffer (%lu bytes)\n", millis(), pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_FONT_ID, 300, "Memory error", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    Serial.printf("[%lu] [XTR] Failed to load page %lu\n", millis(), currentPage);
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_FONT_ID, 300, "Page load error", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        // Column-major, right to left: column index = (width - 1 - x)
        const size_t colIndex = pageWidth - 1 - x;
        const size_t byteInCol = y / 8;
        const size_t bitInByte = 7 - (y % 8);  // MSB = topmost pixel

        const size_t byteOffset = colIndex * colBytes + byteInCol;
        const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
        const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
        const uint8_t pixelValue = (bit1 << 1) | bit2;

        // Threshold: 0=White, 1,2,3=Dark (for best text contrast)
        const bool isBlack = (pixelValue >= 1);

        if (isBlack) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = pagesPerRefresh;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  Serial.printf("[%lu] [XTR] Rendered page %lu/%lu (%u-bit)\n", millis(), currentPage + 1, xtc->getPageCount(),
                bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  File f;
  if (FsHelpers::openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  File f;
  if (FsHelpers::openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      Serial.printf("[%lu] [XTR] Loaded progress: page %lu\n", millis(), currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
