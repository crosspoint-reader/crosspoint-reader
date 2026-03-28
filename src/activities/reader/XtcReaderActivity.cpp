/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long doubleClickWindowMs = 350;
constexpr unsigned long goHomeMs = 1000;
constexpr uint32_t kTenPageSkip = 10;
}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // Long press: skip to next/previous chapter. No-op if the file has no chapters.
  if (SETTINGS.longPressChapterSkip && xtc->hasChapters()) {
    const bool skipFwd = mappedInput.wasLongPressed(MappedInputManager::Button::PageForward, skipChapterMs) ||
                         mappedInput.wasLongPressed(MappedInputManager::Button::Right, skipChapterMs);
    const bool skipBack = mappedInput.wasLongPressed(MappedInputManager::Button::PageBack, skipChapterMs) ||
                          mappedInput.wasLongPressed(MappedInputManager::Button::Left, skipChapterMs);
    if (skipFwd || skipBack) {
      const auto& chapters = xtc->getChapters();
      // Find the chapter containing the current page.
      int chapterIndex = 0;
      for (int i = 0; i < static_cast<int>(chapters.size()); i++) {
        if (currentPage >= chapters[i].startPage && currentPage <= chapters[i].endPage) {
          chapterIndex = i;
          break;
        }
      }
      if (skipFwd && chapterIndex + 1 < static_cast<int>(chapters.size())) {
        currentPage = chapters[chapterIndex + 1].startPage;
      } else if (skipBack && chapterIndex > 0) {
        currentPage = chapters[chapterIndex - 1].startPage;
      }
      requestUpdate();
      return;
    }
  } else if (SETTINGS.longPressChapterSkip) {
    // Consume the long press even with no chapters so the release does not turn a page.
    mappedInput.wasLongPressed(MappedInputManager::Button::PageForward, skipChapterMs);
    mappedInput.wasLongPressed(MappedInputManager::Button::Right, skipChapterMs);
    mappedInput.wasLongPressed(MappedInputManager::Button::PageBack, skipChapterMs);
    mappedInput.wasLongPressed(MappedInputManager::Button::Left, skipChapterMs);
  }

  // Double press: skip 10 pages forward or backward.
  if (SETTINGS.doublePressChapterSkip) {
    const bool skipFwd = mappedInput.wasDoublePressed(MappedInputManager::Button::PageForward, doubleClickWindowMs) ||
                         mappedInput.wasDoublePressed(MappedInputManager::Button::Right, doubleClickWindowMs);
    const bool skipBack = mappedInput.wasDoublePressed(MappedInputManager::Button::PageBack, doubleClickWindowMs) ||
                          mappedInput.wasDoublePressed(MappedInputManager::Button::Left, doubleClickWindowMs);
    if (skipFwd || skipBack) {
      if (skipFwd) {
        currentPage += kTenPageSkip;
        if (currentPage >= xtc->getPageCount()) currentPage = xtc->getPageCount();
      } else {
        currentPage -= (currentPage >= kTenPageSkip) ? kTenPageSkip : currentPage;
      }
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Deferred page turn: fires after the double-press window expires when the button is released.
  if (!prevTriggered && !nextTriggered && SETTINGS.doublePressChapterSkip) {
    if (!mappedInput.isPressed(MappedInputManager::Button::PageBack) &&
        !mappedInput.isPressed(MappedInputManager::Button::Left))
      prevTriggered = mappedInput.wasDoublePressExpired(MappedInputManager::Button::PageBack) ||
                      mappedInput.wasDoublePressExpired(MappedInputManager::Button::Left);
    if (!mappedInput.isPressed(MappedInputManager::Button::PageForward) &&
        !mappedInput.isPressed(MappedInputManager::Button::Right))
      nextTriggered = mappedInput.wasDoublePressExpired(MappedInputManager::Button::PageForward) ||
                      mappedInput.wasDoublePressExpired(MappedInputManager::Button::Right);
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    if (currentPage >= 1) currentPage--;
    requestUpdate();
  } else {
    currentPage++;
    if (currentPage >= xtc->getPageCount()) currentPage = xtc->getPageCount();
    requestUpdate();
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
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
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu", currentPage);
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
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

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
            pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    return;
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
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f, /*silent=*/true)) {
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
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
