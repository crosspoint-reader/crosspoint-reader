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
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <memory>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr size_t XTC_STREAM_CHUNK_LIMIT = 12 * 1024;

struct StreamBuffer {
  std::unique_ptr<uint8_t[]> data;
  size_t bytes = 0;
  uint16_t units = 0;
};

size_t largest8BitBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

StreamBuffer allocateStreamBuffer(size_t bytesPerUnit, uint16_t maxUnits, const char* tag) {
  StreamBuffer out;
  if (bytesPerUnit == 0 || maxUnits == 0 || bytesPerUnit > XTC_STREAM_CHUNK_LIMIT) {
    return out;
  }

  const size_t largest = largest8BitBlock();
  if (largest < XTC_STREAM_CHUNK_LIMIT) {
    LOG_ERR(tag, "No 12 KiB contiguous stream buffer: largest=%u", static_cast<unsigned>(largest));
    return out;
  }

  auto data = makeUniqueNoThrow<uint8_t[]>(XTC_STREAM_CHUNK_LIMIT);
  if (!data) {
    LOG_ERR(tag, "Failed to allocate 12 KiB stream buffer: largest=%u", static_cast<unsigned>(largest));
    return out;
  }

  size_t units = XTC_STREAM_CHUNK_LIMIT / bytesPerUnit;
  if (units > maxUnits) units = maxUnits;

  out.data = std::move(data);
  out.bytes = XTC_STREAM_CHUNK_LIMIT;
  out.units = static_cast<uint16_t>(units);
  LOG_DBG(tag, "Stream buffer: 12 KiB, %u units, largest=%u", static_cast<unsigned>(out.units),
          static_cast<unsigned>(largest));
  return out;
}

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

  clearReaderHeapRecoveryRestart();
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
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  const bool skipPages = !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP &&
                         mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    requestUpdate();
  } else if (nextTriggered) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
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
    clearReaderHeapRecoveryRestart();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (renderPage()) {
    clearReaderHeapRecoveryRestart();
    saveProgress();
  }
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

bool XtcReaderActivity::requestHeapRecoveryOrShowMemoryError(size_t requestedBytes, size_t largestBlock) {
  LOG_ERR("XTR", "Insufficient contiguous heap for stream buffer: requested=%u largest=%u",
          static_cast<unsigned>(requestedBytes), static_cast<unsigned>(largestBlock));
  saveProgress();
  if (silentRestartToReaderForHeapRecovery()) {
    return true;
  }
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
  return false;
}

void XtcReaderActivity::showPageLoadError(xtc::XtcError err) const {
  LOG_ERR("XTR", "Failed to load page %lu: error=%s", currentPage, xtc::errorToString(err));
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

bool XtcReaderActivity::renderXtgPageStreamed(const xtc::PageBitmapLayout& layout) {
  const size_t rowBytes = (layout.width + 7) / 8;
  StreamBuffer chunk = allocateStreamBuffer(rowBytes, layout.height, "XTR");
  if (!chunk.data) {
    requestHeapRecoveryOrShowMemoryError(XTC_STREAM_CHUNK_LIMIT, largest8BitBlock());
    return false;
  }

  renderer.clearScreen();

  for (uint16_t yStart = 0; yStart < layout.height; yStart += chunk.units) {
    const uint16_t rows = static_cast<uint16_t>(std::min<int>(chunk.units, layout.height - yStart));
    const size_t bytes = static_cast<size_t>(rows) * rowBytes;
    const uint32_t offset = static_cast<uint32_t>(static_cast<size_t>(yStart) * rowBytes);
    const xtc::XtcError err = xtc->readPageBitmapRange(layout, offset, chunk.data.get(), bytes);
    if (err != xtc::XtcError::OK) {
      showPageLoadError(err);
      return false;
    }

    for (uint16_t localY = 0; localY < rows; localY++) {
      const uint16_t y = yStart + localY;
      const uint8_t* row = chunk.data.get() + static_cast<size_t>(localY) * rowBytes;
      for (uint16_t x = 0; x < layout.width; x++) {
        const size_t srcByte = x / 8;
        const size_t srcBit = 7 - (x % 8);
        const bool isBlack = !((row[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white
        if (isBlack) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
  }

  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom);
  }

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  LOG_DBG("XTR", "Rendered page %lu/%lu (1-bit streamed, chunk=%u bytes)", currentPage + 1, xtc->getPageCount(),
          static_cast<unsigned>(chunk.bytes));
  return true;
}

bool XtcReaderActivity::renderXthPageStreamed(const xtc::PageBitmapLayout& layout) {
  const size_t colBytes = (layout.height + 7) / 8;
  const size_t planeStride = static_cast<size_t>(layout.width) * colBytes;
  const size_t bytesPerColumnPair = colBytes * 2;
  if (planeStride == 0 || bytesPerColumnPair == 0 || planeStride * 2 > layout.bitmapSize) {
    showPageLoadError(xtc::XtcError::CORRUPTED_HEADER);
    return false;
  }

  StreamBuffer chunk = allocateStreamBuffer(bytesPerColumnPair, layout.width, "XTR");
  if (!chunk.data) {
    requestHeapRecoveryOrShowMemoryError(XTC_STREAM_CHUNK_LIMIT, largest8BitBlock());
    return false;
  }

  const uint16_t columnsPerChunk = chunk.units;
  const uint16_t chunkCount = (layout.width + columnsPerChunk - 1) / columnsPerChunk;
  LOG_DBG("XTR", "XTCH streaming: %ux%u colBytes=%u columns/chunk=%u chunk=%u bytes chunks=%u", layout.width,
          layout.height, static_cast<unsigned>(colBytes), static_cast<unsigned>(columnsPerChunk),
          static_cast<unsigned>(chunk.bytes), static_cast<unsigned>(chunkCount));

  enum class XthPass : uint8_t { Bw, Lsb, Msb, Restore };

  auto renderPass = [&](XthPass pass, uint32_t pixelCounts[4]) -> bool {
    for (uint16_t xStart = 0; xStart < layout.width; xStart += columnsPerChunk) {
      const uint16_t xEnd = static_cast<uint16_t>(std::min<int>(layout.width, xStart + columnsPerChunk));
      const uint16_t columns = xEnd - xStart;
      const size_t planeBytes = static_cast<size_t>(columns) * colBytes;
      const size_t fileColStart = layout.width - xEnd;
      const uint32_t plane1Offset = static_cast<uint32_t>(fileColStart * colBytes);
      const uint32_t plane2Offset = static_cast<uint32_t>(planeStride + fileColStart * colBytes);

      xtc::XtcError err = xtc->readPageBitmapRange(layout, plane1Offset, chunk.data.get(), planeBytes);
      if (err != xtc::XtcError::OK) {
        showPageLoadError(err);
        return false;
      }
      err = xtc->readPageBitmapRange(layout, plane2Offset, chunk.data.get() + planeBytes, planeBytes);
      if (err != xtc::XtcError::OK) {
        showPageLoadError(err);
        return false;
      }

      const uint8_t* plane1 = chunk.data.get();
      const uint8_t* plane2 = chunk.data.get() + planeBytes;

      for (uint16_t y = 0; y < layout.height; y++) {
        const size_t byteInCol = y / 8;
        const uint8_t bitInByte = 7 - (y % 8);
        for (uint16_t x = xStart; x < xEnd; x++) {
          const size_t fileCol = layout.width - 1 - x;
          const size_t localCol = fileCol - fileColStart;
          const size_t byteOffset = localCol * colBytes + byteInCol;
          const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
          const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
          const uint8_t pixelValue = (bit1 << 1) | bit2;

          switch (pass) {
            case XthPass::Bw:
              pixelCounts[pixelValue]++;
              if (pixelValue >= 1) renderer.drawPixel(x, y, true);
              break;
            case XthPass::Lsb:
              if (pixelValue == 1) renderer.drawPixel(x, y, false);
              break;
            case XthPass::Msb:
              if (pixelValue == 1 || pixelValue == 2) renderer.drawPixel(x, y, false);
              break;
            case XthPass::Restore:
              if (pixelValue >= 1) renderer.drawPixel(x, y, true);
              break;
          }
        }
      }
    }
    return true;
  };

  uint32_t pixelCounts[4] = {0, 0, 0, 0};

  renderer.clearScreen();
  if (!renderPass(XthPass::Bw, pixelCounts)) return false;
  LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
          pixelCounts[1], pixelCounts[2], pixelCounts[3]);
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  renderer.clearScreen(0x00);
  if (!renderPass(XthPass::Lsb, pixelCounts)) return false;
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  if (!renderPass(XthPass::Msb, pixelCounts)) return false;
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();

  renderer.clearScreen();
  if (!renderPass(XthPass::Restore, pixelCounts)) return false;
  renderer.cleanupGrayscaleWithFrameBuffer();

  LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit streamed grayscale)", currentPage + 1, xtc->getPageCount());
  return true;
}

bool XtcReaderActivity::renderPage() {
  xtc::PageBitmapLayout layout;
  xtc::XtcError err = xtc->beginPageBitmapRead(currentPage, layout);
  if (err != xtc::XtcError::OK) {
    xtc->endPageBitmapRead();
    showPageLoadError(err);
    return false;
  }
  const ScopedCleanup cleanup{[this]() {
    if (xtc) xtc->endPageBitmapRead();
  }};

  bool ok = false;
  if (layout.bitDepth == 2) {
    ok = renderXthPageStreamed(layout);
  } else {
    ok = renderXtgPageStreamed(layout);
  }
  return ok;
}

void XtcReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
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
  HalFile f;
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

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
