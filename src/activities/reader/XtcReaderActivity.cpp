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

#include <algorithm>
#include <cstdlib>

#include "BookRelocation.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FinishedBookActivity.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  // See ReaderUtils::InputDrainGuard — prevents wake-up power-button hold from leaking into
  // the first page-turn check as a page turn or chapter skip.
  inputDrainGuard.arm();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), "",
                       ReaderActivity::coverThumbPlaceholder(xtc->getPath()));

  // Start the reading-stats session. XTC has real title/author from the
  // file header so the per-book screen will look nicer than TXT/MD.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(xtc->getPath()), xtc->getTitle(),
                                      xtc->getAuthor());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  // Same rule as EpubReaderActivity::onExit(). A 2-bit XTC page goes out through the grayscale
  // plane pass (renderPage, bitDepth == 2), which leaves grey on the glass that the controller's
  // B/W baseline does not describe; and on a graded-canvas panel (supportsGrayFrame -- the T5S3)
  // even a 1-bit page leaves the canvas holding it. Either way the next screen's FAST diff runs
  // against a frame that is not what is on the panel. The EPUB reader's comment records the
  // device symptom. Until now this reader armed the refresh only on its submenu-launch paths,
  // never on exit. Before xtc.reset() below, which this reads.
  if ((xtc && xtc->getBitDepth() == 2) || renderer.supportsGrayFrame()) {
    ReaderUtils::enforceExitFullRefresh(renderer);
  }

  // Flush stats session before tearing down the XTC reader.
  globalReadingSessionTracker().end();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  UITheme::getInstance().getMutableTheme().onBookWillClose(xtc ? xtc->getPath() : "", nullptr, xtc.get(), nullptr);
  xtc.reset();
  // Last, as in EpubReaderActivity::onExit().
  BookRelocation::bookClosed();
}

bool XtcReaderActivity::openChapterSelection() {
  if (!xtc || !xtc->hasChapters() || xtc->getChapters().empty()) return false;
  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             currentPage = std::get<PageResult>(result.data).page;
                           }
                         });
  return true;
}

void XtcReaderActivity::loop() {
  if (inputDrainGuard.shouldDrain(mappedInput)) {
    buttonEvents.drain();
    return;
  }

  bool buttonPrevTurn = false;
  bool buttonNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      if (openChapterSelection()) return;
    }

    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        finish();
        return;
      }
    }

    // Built-in default for a long-press on a page-turn button is chapter skip.
    // onButtonAction() no-ops when the XTC has no chapters. The FSM emits no Short for a
    // press that already produced a Long, so the release cannot also turn the page.
    // (Mirrors EpubReaderActivity; non-default long actions are dispatched by main.cpp.)
    if (ev.type == ButtonEventManager::PressType::Long) {
      const bool prevChapter =
          (ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnLongPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnLongLeft == BA::BTN_DEFAULT);
      const bool nextChapter =
          (ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnLongPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnLongRight == BA::BTN_DEFAULT);
      if (prevChapter || nextChapter) {
        onButtonAction(nextChapter ? BA::BTN_NEXT_SECTION : BA::BTN_PREV_SECTION);
        return;
      }
    }

    // Page turns for all four navigation buttons come from the event queue (see
    // EpubReaderActivity::loop for why). A non-default short action never arrives here —
    // main.cpp dispatches it globally — but the setting is re-checked so a future caller
    // that pushes events directly cannot turn a remapped button into a page turn.
    if (ev.type == ButtonEventManager::PressType::Short) {
      if ((ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnShortPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnShortLeft == BA::BTN_DEFAULT)) {
        buttonPrevTurn = true;
        continue;
      }
      if ((ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnShortPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnShortRight == BA::BTN_DEFAULT)) {
        buttonNextTurn = true;
        continue;
      }
    }
  }

  // A centre-third tap, or the top-edge menu swipe, opens the chapter list -- this reader's
  // whole overlay, and what its Confirm already does. The EPUB reader answers the same gesture
  // with its reader menu; each reader offers the navigation it has, so the gesture means
  // "show me where I am in the book" wherever it is made.
  //
  // Kept next to the page-turn zones as it is there, and for the same reason: the zones do not
  // overlap (outer thirds vs centre), so the order is not load-bearing.
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput) && openChapterSelection()) return;

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectTiltPageTurn();
  // Touch page turns join tilt here rather than in the button event queue: both
  // are gesture sources the queue does not carry, and both must lose to an
  // explicit button press. Inert on non-touch boards and when the user has set
  // touchReaderControls to Off.
  const ReaderUtils::TouchPageTurn touchTurn = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  prevTriggered = prevTriggered || touchTurn.prev;
  nextTriggered = nextTriggered || touchTurn.next;
  if (!prevTriggered && !nextTriggered) {
    if (!buttonPrevTurn && !buttonNextTurn) {
      return;
    }
    prevTriggered = buttonPrevTurn;
    nextTriggered = buttonNextTurn;
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button opens the finished-book flow and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      saveProgress();
      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, xtc->getPath(), std::string(), std::string(),
                                           xtc->getAuthor());
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  if (prevTriggered) {
    if (currentPage > 0) {
      currentPage--;
      globalReadingSessionTracker().onPageTurn();
      requestUpdate();
    }
  } else if (nextTriggered) {
    currentPage++;
    globalReadingSessionTracker().onPageTurn();
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

  // Stream the page row-major instead of allocating the whole page in heap. A
  // full XTH page (e.g. 96000 bytes) does not fit the fragmented ESP32-C3 heap
  // and previously failed to render; the stream keeps the working set to a few
  // small bounded buffers (XTH is transposed once to a row-major temp file).
  xtc::XtcPageRowStream pageStream;
  if (!xtc->openPageRowStream(pageStream, currentPage)) {
    LOG_ERR("XTR", "Failed to open page stream for page %lu", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // XTC/XTCH pages are pre-rendered for the device. Ignore logical reader
  // orientation and map file pixels directly to the physical panel, cropping
  // if the encoded page is larger than this device (X4: 480x800, X3: 528x792).
  const uint16_t maxSrcX = std::min(pageWidth, renderer.getDisplayHeight());
  const uint16_t maxSrcY = std::min(pageHeight, renderer.getDisplayWidth());
  const size_t rowBytes = (static_cast<size_t>(pageWidth) + 7) / 8;
  uint8_t* row = static_cast<uint8_t*>(malloc(rowBytes));  // freed before return
  if (!row) {
    LOG_ERR("XTR", "Failed to allocate XTC row buffer (%u bytes)", static_cast<unsigned int>(rowBytes));
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (bitDepth == 2) {
    // XTH 2-bit mode (grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black).
    // The stream splits the page into a cheap 1-bit ink plane (BW passes) and a
    // 2-bit gray plane (LSB/MSB passes), so the two identical BW passes only
    // touch the small plane. Right-to-left column order and bit-plane decoding
    // are handled inside the stream.

    // Pass 1: BW buffer - draw all ink (non-white) pixels as black.
    for (uint16_t y = 0; y < maxSrcY; y++) {
      bool invertBits = false;
      if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX, invertBits);
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < maxSrcY; y++) {
      if (!pageStream.readGrayMaskRow(y, xtc::XtcPageRowStream::GrayMask::DarkOnly, row, rowBytes, maxSrcX)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX);
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < maxSrcY; y++) {
      if (!pageStream.readGrayMaskRow(y, xtc::XtcPageRowStream::GrayMask::LightOrDark, row, rowBytes, maxSrcX)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX);
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < maxSrcY; y++) {
      bool invertBits = false;
      if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX, invertBits);
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(row);
    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: ink (non-white) pixels drawn black.
    for (uint16_t y = 0; y < maxSrcY; y++) {
      bool invertBits = false;
      if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX, invertBits);
    }
  }
  // White pixels are already cleared by clearScreen()

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  free(row);
  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    const uint8_t percent =
        ReaderUtils::pageProgressPercentByte(static_cast<int>(currentPage), static_cast<int>(xtc->getPageCount()));
    uint8_t data[5];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    data[4] = percent;
    f.write(data, 5);
    f.close();
    globalReadingSessionTracker().updateProgress(percent);
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

bool XtcReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Xtc xtc(filePath, "/.crosspoint");
  if (!xtc.load()) {
    LOG_DBG("SLP", "XTC: failed to load %s", filePath.c_str());
    return false;
  }

  // Load saved page number
  uint32_t savedPage = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      savedPage = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }
    f.close();
  }
  if (savedPage >= xtc.getPageCount()) savedPage = 0;

  const uint16_t pageWidth = xtc.getPageWidth();
  const uint16_t pageHeight = xtc.getPageHeight();
  const uint8_t bitDepth = xtc.getBitDepth();

  // Stream the page (BW pass only; grayscale is not needed under the overlay)
  // instead of allocating the whole page, which can fail on the fragmented heap.
  (void)bitDepth;
  xtc::XtcPageRowStream pageStream;
  if (!xtc.openPageRowStream(pageStream, savedPage, /*persist=*/false)) {
    LOG_ERR("SLP", "XTC: failed to open page stream for page %lu", savedPage);
    return false;
  }

  renderer.clearScreen();

  const uint16_t maxX = std::min(pageWidth, renderer.getDisplayHeight());
  const uint16_t maxY = std::min(pageHeight, renderer.getDisplayWidth());
  const size_t rowBytes = (static_cast<size_t>(pageWidth) + 7) / 8;
  uint8_t* row = static_cast<uint8_t*>(malloc(rowBytes));  // freed before return
  if (!row) {
    LOG_ERR("SLP", "XTC: failed to allocate row buffer (%u bytes)", static_cast<unsigned int>(rowBytes));
    return false;
  }

  // Both formats: stream.isInk() means a non-white pixel -> draw black.
  for (uint16_t y = 0; y < maxY; y++) {
    bool invertBits = false;
    if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
      free(row);
      return false;
    }
    renderer.writePhysicalPortraitPackedRow(y, row, maxX, invertBits);
  }

  free(row);
  return true;
}

void XtcReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  if (!xtc) return;
  const uint32_t pageCount = xtc->getPageCount();
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      if (currentPage + 1 < pageCount) {
        currentPage++;
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    case BA::BTN_PAGE_BACK:
      if (currentPage > 0) {
        currentPage--;
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    case BA::BTN_PAGE_FORWARD_10: {
      const uint32_t prevPage = currentPage;
      currentPage = (currentPage + 10 < pageCount) ? currentPage + 10 : pageCount - 1;
      if (currentPage != prevPage) {
        globalReadingSessionTracker().onPageTurn();
      }
      requestUpdate();
      break;
    }
    case BA::BTN_PAGE_BACK_10: {
      const uint32_t prevPage = currentPage;
      currentPage = (currentPage >= 10) ? currentPage - 10 : 0;
      if (currentPage != prevPage) {
        globalReadingSessionTracker().onPageTurn();
      }
      requestUpdate();
      break;
    }
    case BA::BTN_NEXT_SECTION:
      if (xtc->hasChapters()) {
        const auto& chapters = xtc->getChapters();
        const auto it = std::find_if(chapters.begin(), chapters.end(),
                                     [this](const auto& ch) { return ch.startPage > currentPage; });
        if (it != chapters.end()) {
          currentPage = it->startPage;
          globalReadingSessionTracker().onPageTurn();
          requestUpdate();
        }
      }
      break;
    case BA::BTN_PREV_SECTION:
      if (xtc->hasChapters()) {
        const auto& chapters = xtc->getChapters();
        const auto prevChapter = std::find_if(chapters.rbegin(), chapters.rend(),
                                              [this](const auto& ch) { return ch.startPage < currentPage; });

        if (prevChapter != chapters.rend()) {
          currentPage = prevChapter->startPage;
          globalReadingSessionTracker().onPageTurn();
          requestUpdate();
        }
      }
      break;
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      finish();
      break;
    // The chapter list is the whole of this reader's navigation, so it answers both names: a
    // gesture bound to "Table of contents" and one bound to "Reader menu" find the same thing,
    // rather than one of them silently doing nothing here.
    case BA::BTN_OPEN_TOC:
    case BA::BTN_READER_MENU:
      openChapterSelection();
      break;
    default:
      break;
  }
}

// Re-render the current page rather than let main.cpp re-flush it: a 2-bit page carries a gray
// overlay that a raw flush of the B/W buffer would strip. The override replaces whatever the
// refresh cycle picks for the base, so the requested mode also holds with the cycle set to Never.
bool XtcReaderActivity::handleForcedRefresh(const HalDisplay::RefreshMode mode) {
  if (!xtc) return false;  // render() would draw nothing and leave the override armed
  renderer.setNextDisplayRefreshMode(mode);
  requestUpdate();
  return true;
}
