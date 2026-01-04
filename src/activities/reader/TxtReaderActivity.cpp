#include "TxtReaderActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int topPadding = 10;
constexpr int horizontalPadding = 15;
constexpr int statusBarMargin = 25;
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
}  // namespace

void TxtReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtReaderActivity*>(param);
  self->displayTaskLoop();
}

void TxtReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!txt) {
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

  txt->setupCacheDir();

  // Save current txt as last opened file
  APP_STATE.openEpubPath = txt->getPath();
  APP_STATE.saveToFile();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&TxtReaderActivity::taskTrampoline, "TxtReaderActivityTask",
              6144,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void TxtReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  pageOffsets.clear();
  currentPageLines.clear();
  txt.reset();
}

void TxtReaderActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Long press BACK (1s+) goes directly to home
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  // Short press BACK goes to file selection
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoBack();
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (!prevReleased && !nextReleased) {
    return;
  }

  if (prevReleased && currentPage > 0) {
    currentPage--;
    updateRequired = true;
  } else if (nextReleased && currentPage < totalPages - 1) {
    currentPage++;
    updateRequired = true;
  }
}

void TxtReaderActivity::displayTaskLoop() {
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

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Calculate viewport dimensions
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += topPadding;
  orientedMarginLeft += horizontalPadding;
  orientedMarginRight += horizontalPadding;
  orientedMarginBottom += statusBarMargin;

  viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  Serial.printf("[%lu] [TRS] Viewport: %dx%d, lines per page: %d\n", millis(), viewportWidth, viewportHeight,
                linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();
  int lastProgressPercent = -1;

  Serial.printf("[%lu] [TRS] Building page index for %zu bytes...\n", millis(), fileSize);

  // Progress bar dimensions (matching EpubReaderActivity style)
  constexpr int barWidth = 200;
  constexpr int barHeight = 10;
  constexpr int boxMargin = 20;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, "Indexing...");
  const int boxWidth = (barWidth > textWidth ? barWidth : textWidth) + boxMargin * 2;
  const int boxHeight = renderer.getLineHeight(UI_12_FONT_ID) + barHeight + boxMargin * 3;
  const int boxX = (renderer.getScreenWidth() - boxWidth) / 2;
  constexpr int boxY = 50;
  const int barX = boxX + (boxWidth - barWidth) / 2;
  const int barY = boxY + renderer.getLineHeight(UI_12_FONT_ID) + boxMargin * 2;

  // Draw initial progress box
  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);
  renderer.drawText(UI_12_FONT_ID, boxX + boxMargin, boxY + boxMargin, "Indexing...");
  renderer.drawRect(boxX + 5, boxY + 5, boxWidth - 10, boxHeight - 10);
  renderer.drawRect(barX, barY, barWidth, barHeight);
  renderer.displayBuffer();

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Update progress bar every 2%
    int progressPercent = (offset * 100) / fileSize;
    if (progressPercent != lastProgressPercent && progressPercent % 2 == 0) {
      lastProgressPercent = progressPercent;

      // Fill progress bar
      const int fillWidth = (barWidth - 2) * progressPercent / 100;
      renderer.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, true);
      renderer.displayBuffer(EInkDisplay::FAST_REFRESH);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  Serial.printf("[%lu] [TRS] Built page index: %d pages\n", millis(), totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    Serial.printf("[%lu] [TRS] Failed to allocate %zu bytes\n", millis(), chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Parse lines from buffer
  size_t pos = 0;
  size_t bytesConsumed = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Extract line (without newline)
    std::string line(reinterpret_cast<char*>(buffer + pos), lineEnd - pos);

    // Remove carriage return if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Word wrap if needed
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      int lineWidth = renderer.getTextWidth(SETTINGS.getReaderFontId(), line.c_str());

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 &&
             renderer.getTextWidth(SETTINGS.getReaderFontId(), line.substr(0, breakPos).c_str()) > viewportWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      if (breakPos < line.length() && line[breakPos] == ' ') {
        breakPos++;
      }
      line = line.substr(breakPos);
    }

    // If we still have remaining wrapped text but no room, don't consume this source line
    if (!line.empty() && static_cast<int>(outLines.size()) >= linesPerPage) {
      break;
    }

    // Move past the newline
    bytesConsumed = lineEnd + 1;
    pos = lineEnd + 1;
  }

  // Handle case where we filled the page mid-line (word wrap)
  if (bytesConsumed == 0 && !outLines.empty()) {
    // We processed some wrapped content, estimate bytes consumed
    // This is approximate - we need to track actual byte positions
    bytesConsumed = pos;
  }

  nextOffset = offset + (bytesConsumed > 0 ? bytesConsumed : chunkSize);
  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::renderScreen() {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Indexing...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty file", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += topPadding;
  orientedMarginLeft += horizontalPadding;
  orientedMarginRight += horizontalPadding;
  orientedMarginBottom += statusBarMargin;

  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());

  int y = orientedMarginTop;
  for (const auto& line : currentPageLines) {
    if (!line.empty()) {
      renderer.drawText(SETTINGS.getReaderFontId(), orientedMarginLeft, y, line.c_str());
    }
    y += lineHeight;
  }

  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

void TxtReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) const {
  const bool showProgress = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;

  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  if (showProgress) {
    const int progress = totalPages > 0 ? (currentPage + 1) * 100 / totalPages : 0;
    const std::string progressStr =
        std::to_string(currentPage + 1) + "/" + std::to_string(totalPages) + "  " + std::to_string(progress) + "%";
    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr.c_str());
  }

  if (showBattery) {
    ScreenComponents::drawBattery(renderer, orientedMarginLeft, textY);
  }

  if (showTitle) {
    const int titleMarginLeft = 50 + 30 + orientedMarginLeft;
    const int titleMarginRight = progressTextWidth + 30 + orientedMarginRight;
    const int availableTextWidth = renderer.getScreenWidth() - titleMarginLeft - titleMarginRight;

    std::string title = txt->getTitle();
    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    while (titleWidth > availableTextWidth && title.length() > 11) {
      title.replace(title.length() - 8, 8, "...");
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID, titleMarginLeft + (availableTextWidth - titleWidth) / 2, textY, title.c_str());
  }
}

void TxtReaderActivity::saveProgress() const {
  FsFile f;
  if (SdMan.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
    f.close();
  }
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (SdMan.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      Serial.printf("[%lu] [TRS] Loaded progress: page %d/%d\n", millis(), currentPage, totalPages);
    }
    f.close();
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format:
  // - 4 bytes: magic "TXTI"
  // - 4 bytes: file size (to validate cache)
  // - 4 bytes: viewport width
  // - 4 bytes: lines per page
  // - 4 bytes: total pages count
  // - N * 4 bytes: page offsets (size_t stored as uint32_t)

  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!SdMan.openFileForRead("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] No page index cache found\n", millis());
    return false;
  }

  // Read and validate header
  uint8_t header[20];
  if (f.read(header, 20) != 20) {
    f.close();
    return false;
  }

  // Check magic
  if (header[0] != 'T' || header[1] != 'X' || header[2] != 'T' || header[3] != 'I') {
    f.close();
    return false;
  }

  // Check file size matches
  uint32_t cachedFileSize = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
  if (cachedFileSize != txt->getFileSize()) {
    Serial.printf("[%lu] [TRS] Cache file size mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  // Check viewport width matches
  uint32_t cachedViewportWidth = header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);
  if (static_cast<int>(cachedViewportWidth) != viewportWidth) {
    Serial.printf("[%lu] [TRS] Cache viewport width mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  // Check lines per page matches
  uint32_t cachedLinesPerPage = header[12] | (header[13] << 8) | (header[14] << 16) | (header[15] << 24);
  if (static_cast<int>(cachedLinesPerPage) != linesPerPage) {
    Serial.printf("[%lu] [TRS] Cache lines per page mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  // Read total pages
  uint32_t cachedTotalPages = header[16] | (header[17] << 8) | (header[18] << 16) | (header[19] << 24);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(cachedTotalPages);

  for (uint32_t i = 0; i < cachedTotalPages; i++) {
    uint8_t offsetData[4];
    if (f.read(offsetData, 4) != 4) {
      f.close();
      pageOffsets.clear();
      return false;
    }
    uint32_t offset = offsetData[0] | (offsetData[1] << 8) | (offsetData[2] << 16) | (offsetData[3] << 24);
    pageOffsets.push_back(offset);
  }

  f.close();
  totalPages = pageOffsets.size();
  Serial.printf("[%lu] [TRS] Loaded page index cache: %d pages\n", millis(), totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!SdMan.openFileForWrite("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] Failed to save page index cache\n", millis());
    return;
  }

  // Write header
  uint8_t header[20];
  header[0] = 'T';
  header[1] = 'X';
  header[2] = 'T';
  header[3] = 'I';

  // File size
  uint32_t fileSize = txt->getFileSize();
  header[4] = fileSize & 0xFF;
  header[5] = (fileSize >> 8) & 0xFF;
  header[6] = (fileSize >> 16) & 0xFF;
  header[7] = (fileSize >> 24) & 0xFF;

  // Viewport width
  header[8] = viewportWidth & 0xFF;
  header[9] = (viewportWidth >> 8) & 0xFF;
  header[10] = (viewportWidth >> 16) & 0xFF;
  header[11] = (viewportWidth >> 24) & 0xFF;

  // Lines per page
  header[12] = linesPerPage & 0xFF;
  header[13] = (linesPerPage >> 8) & 0xFF;
  header[14] = (linesPerPage >> 16) & 0xFF;
  header[15] = (linesPerPage >> 24) & 0xFF;

  // Total pages
  uint32_t numPages = pageOffsets.size();
  header[16] = numPages & 0xFF;
  header[17] = (numPages >> 8) & 0xFF;
  header[18] = (numPages >> 16) & 0xFF;
  header[19] = (numPages >> 24) & 0xFF;

  f.write(header, 20);

  // Write page offsets
  for (size_t offset : pageOffsets) {
    uint8_t offsetData[4];
    offsetData[0] = offset & 0xFF;
    offsetData[1] = (offset >> 8) & 0xFF;
    offsetData[2] = (offset >> 16) & 0xFF;
    offsetData[3] = (offset >> 24) & 0xFF;
    f.write(offsetData, 4);
  }

  f.close();
  Serial.printf("[%lu] [TRS] Saved page index cache: %d pages\n", millis(), totalPages);
}
