#include "LineReaderActivity.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>

#include <algorithm>

#include "CrossPointState.h"
#include "FinishedBookActivity.h"
#include "KOReaderDocumentId.h"
#include "ReaderActivity.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "activities/home/BookDetails.h"
#include "components/UITheme.h"

void LineReaderActivity::onEnter() {
  Activity::onEnter();

  // See ReaderUtils::InputDrainGuard — prevents wake-up power-button hold from leaking into
  // the first page-turn check as a page turn or chapter skip.
  inputDrainGuard.arm();

  if (!txt) {
    return;
  }

  // The scaled-glyph cache, sized for this text's font (see EpubReaderActivity::onEnter).
  renderer.ensureScaledGlyphCache(SETTINGS.getTxtReaderFontId());

  {
    RenderLock lock(*this);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }

  txt->setupCacheDir();
  onReaderEnter();

  // Save as last opened file and add to recent books
  const std::string filePath = txt->getPath();
  const std::string fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  // A text book has no metadata of its own: it is listed under its .opf sidecar's title and author
  // when it has one (the web interface's Details dialog writes it), and its file name otherwise.
  BookDetails details;
  if (!BookDetailsLookup::cached(filePath, 0, details)) BookDetailsLookup::parse(filePath, 0, details);
  const std::string title = details.title.empty() ? fileName : details.title;
  const std::string series = !details.series.empty() && !details.seriesIndex.empty()
                                 ? details.series + " #" + details.seriesIndex
                                 : details.series;
  RECENT_BOOKS.addBook(filePath, title, details.author, series, ReaderActivity::coverThumbPlaceholder(filePath));

  // Start reading-stats session. Same filename-hash policy as EPUB, so the history is keyed by the
  // file name; BookRelocation carries it over when the file is renamed.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(filePath), title, details.author);

  // Trigger first update
  requestUpdate();
}

void LineReaderActivity::onExit() {
  Activity::onExit();

  // Same rule as EpubReaderActivity::onExit(), minus its anti-aliasing clause (these readers have
  // none): on a panel where EVERY push goes through a graded canvas (supportsGrayFrame -- the
  // T5S3), a B/W page leaves that canvas holding the page and the next screen's FAST diff runs
  // against it. The EPUB reader's comment records the device symptom: the last page and Home
  // superimposed, settling a refresh later. Until now only the EPUB reader armed this; the TXT and
  // MD readers only did so on their submenu-launch paths, never on exit.
  if (renderer.supportsGrayFrame()) {
    ReaderUtils::enforceExitFullRefresh(renderer);
  }

  // Glyph caches back before the stats flush, as EpubReaderActivity::onExit() does.
  renderer.releaseGlyphCaches();

  // Flush the stats session before tearing down the document — same pattern as
  // EpubReaderActivity::onExit().
  globalReadingSessionTracker().end();

  onReaderExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  UITheme::getInstance().getMutableTheme().onBookWillClose(txt ? txt->getPath() : "", nullptr, nullptr, txt.get());
  txt.reset();
}

void LineReaderActivity::loop() {
  const bool drainActive = inputDrainGuard.shouldDrain(mappedInput);
  if (!drainActive && drainWasActive) {
    halTiltSensor.clearPendingEvents();
  }
  drainWasActive = drainActive;

  if (drainActive) {
    buttonEvents.drain();
    return;
  }

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
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

    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      if (onConfirmShortPress()) {
        return;
      }
      continue;
    }

    if ((ev.button == MappedInputManager::Button::PageBack || ev.button == MappedInputManager::Button::Left) &&
        ev.type == ButtonEventManager::PressType::Short) {
      goToPreviousPage();
      return;
    }

    if ((ev.button == MappedInputManager::Button::PageForward || ev.button == MappedInputManager::Button::Right) &&
        ev.type == ButtonEventManager::PressType::Short) {
      goToNextPage();
      return;
    }
  }

  // A centre-third tap, or the top-edge menu swipe, opens this reader's own overlay: the heading
  // list in MD, and nothing at all in plain TXT, which has none -- exactly what its Confirm
  // does, reached through the same call so the two cannot diverge. The EPUB reader answers the
  // same gesture with its reader menu; each reader offers the navigation it has.
  //
  // Kept next to the page-turn zones as it is there, and for the same reason: the zones do not
  // overlap (outer thirds vs centre), so the order is not load-bearing.
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput) && onConfirmShortPress()) return;

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectTiltPageTurn();
  // Touch page turns join tilt here rather than in the button event queue: both
  // are gesture sources the queue does not carry, and both must lose to an
  // explicit button press. Inert on non-touch boards and when the user has set
  // touchReaderControls to Off.
  const ReaderUtils::TouchPageTurn touchTurn = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  prevTriggered = prevTriggered || touchTurn.prev;
  nextTriggered = nextTriggered || touchTurn.next;
  if (prevTriggered) {
    goToPreviousPage();
  } else if (nextTriggered) {
    goToNextPage();
  }
}

void LineReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (action) {
    // Routed through the same helpers the buttons use, so the end-of-book handoff to the
    // finished-book flow and the session bookkeeping happen for a gesture too.
    case BA::BTN_PAGE_FORWARD:
      goToNextPage();
      break;
    case BA::BTN_PAGE_BACK:
      goToPreviousPage();
      break;
    case BA::BTN_PAGE_FORWARD_10: {
      // max(0, ...) because totalPages is 0 for an empty file (MdReaderActivity::buildPageIndex
      // returns early on a zero-byte document), and the naive clamp to totalPages - 1 would
      // land currentPage on -1 and render off the end of pageOffsets.
      const int target = std::min(currentPage + 10, std::max(0, totalPages - 1));
      if (target != currentPage) {
        currentPage = target;
        onPageChanged();
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    }
    case BA::BTN_PAGE_BACK_10: {
      const int target = std::max(currentPage - 10, 0);
      if (target != currentPage) {
        currentPage = target;
        onPageChanged();
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    }
    // Whatever overlay this reader has -- MD's heading list, TXT's starred pages -- under both
    // names, so neither binding is silently inert here. A reader with none simply ignores it.
    case BA::BTN_OPEN_TOC:
    case BA::BTN_READER_MENU:
      onConfirmShortPress();
      break;
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      finish();
      break;
    default:
      break;
  }
}

// Re-render the current page rather than let main.cpp re-flush it: with text anti-aliasing on, a
// raw flush sends only the B/W buffer, so the refresh meant to clean the page up would strip its
// gray planes until the next turn. The override replaces whatever the refresh cycle picks, so the
// requested mode also holds with the cycle set to Never.
bool LineReaderActivity::handleForcedRefresh(const HalDisplay::RefreshMode mode) {
  if (!txt) return false;  // render() would draw nothing and leave the override armed
  renderer.setNextDisplayRefreshMode(mode);
  requestUpdate();
  return true;
}

void LineReaderActivity::goToPreviousPage() {
  if (currentPage > 0) {
    currentPage--;
    onPageChanged();
    globalReadingSessionTracker().onPageTurn();
    requestUpdate();
  }
}

void LineReaderActivity::goToNextPage() {
  if (currentPage < totalPages - 1) {
    currentPage++;
    onPageChanged();
    globalReadingSessionTracker().onPageTurn();
    requestUpdate();
  } else {
    launchFinishedBookFlow();
  }
}

void LineReaderActivity::launchFinishedBookFlow() {
  saveProgress();
  BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, txt ? txt->getPath() : std::string(),
                                       std::string(), std::string());
}

LineReaderActivity::TextLayout LineReaderActivity::computeTextLayout(const GfxRenderer& renderer, const int fontId,
                                                                     const uint8_t screenMargin) {
  TextLayout layout;
  renderer.getOrientedViewableTRBL(&layout.marginTop, &layout.marginRight, &layout.marginBottom, &layout.marginLeft);
  layout.marginTop += std::max(static_cast<int>(screenMargin), UITheme::getStatusBarTopHeight());
  layout.marginLeft += screenMargin;
  layout.marginRight += screenMargin;
  layout.marginBottom += std::max(static_cast<int>(screenMargin), UITheme::getStatusBarBottomHeight());
  layout.viewportWidth = renderer.getScreenWidth() - layout.marginLeft - layout.marginRight;
  layout.viewportHeight = renderer.getScreenHeight() - layout.marginTop - layout.marginBottom;
  layout.linesPerPage = std::max(1, layout.viewportHeight / renderer.getLineHeight(fontId));
  return layout;
}

void LineReaderActivity::computeViewportLayout() {
  // Store current settings for index-cache validation
  cachedFontId = SETTINGS.getTxtReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  const TextLayout layout = computeTextLayout(renderer, cachedFontId, cachedScreenMargin);
  cachedOrientedMarginTop = layout.marginTop;
  cachedOrientedMarginRight = layout.marginRight;
  cachedOrientedMarginBottom = layout.marginBottom;
  cachedOrientedMarginLeft = layout.marginLeft;
  viewportWidth = layout.viewportWidth;
  linesPerPage = layout.linesPerPage;

  LOG_DBG(logTag, "Viewport: %dx%d, lines per page: %d", viewportWidth, layout.viewportHeight, linesPerPage);
}

void LineReaderActivity::noteStatusBarRendered() const {
  lastStatusBarPage = currentPage + 1;
  lastStatusBarBattery = SETTINGS.statusBarBattery ? static_cast<int>(powerManager.getBatteryPercentage()) : -1;
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    lastStatusBarClockMinute = now > 0 ? static_cast<int>(now / 60) : -1;
  } else {
    lastStatusBarClockMinute = -1;
  }
}

bool LineReaderActivity::shouldSkipPeriodicUpdate() const {
  if (lastStatusBarPage < 0) return false;
  if (currentPage + 1 != lastStatusBarPage) return false;
  if (SETTINGS.statusBarBattery) {
    if (static_cast<int>(powerManager.getBatteryPercentage()) != lastStatusBarBattery) return false;
  }
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    const int minute = now > 0 ? static_cast<int>(now / 60) : -1;
    if (minute != lastStatusBarClockMinute) return false;
  }
  return true;
}

void LineReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite(logTag, txt->getCachePath() + "/progress.bin", f)) {
    const size_t offset =
        (currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size())) ? pageOffsets[currentPage] : 0;
    const uint8_t percent = ReaderUtils::pageProgressPercentByte(currentPage, totalPages);
    uint8_t data[7];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = offset & 0xFF;
    data[3] = (offset >> 8) & 0xFF;
    data[4] = (offset >> 16) & 0xFF;
    data[5] = (offset >> 24) & 0xFF;
    data[6] = percent;
    f.write(data, 7);
    f.close();
    globalReadingSessionTracker().updateProgress(percent);
  }
}

void LineReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead(logTag, txt->getCachePath() + "/progress.bin", f)) {
    // Page sits in bytes 0-1 in both the legacy 4-byte format and the current
    // 7-byte format (page counts stay well under 65536).
    uint8_t data[2];
    if (f.read(data, 2) == 2) {
      const int loadedPage = data[0] + (data[1] << 8);
      if (totalPages <= 0) {
        currentPage = 0;
      } else {
        currentPage = std::min(loadedPage, totalPages - 1);
      }
      LOG_DBG(logTag, "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}
