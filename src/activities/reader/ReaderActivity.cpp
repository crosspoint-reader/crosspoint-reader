#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <optional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderDocument.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderDocument.h"
#include "Xtc.h"
#include "XtcReaderDocument.h"
#include "activities/util/BmpViewerActivity.h"
#include "components/UITheme.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
}

bool ReaderActivity::isImageFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

int ReaderActivity::initialRefreshCountdown() const {
  if (!allowFastInitialRefresh) return 0;
  const int refreshFrequency = SETTINGS.getRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

std::unique_ptr<ReaderDocument> ReaderActivity::createDocument(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  if (isXtcFile(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("READER", "Failed to allocate XTC object");
      return nullptr;
    }
    if (!xtc->load()) {
      LOG_ERR("READER", "Failed to load XTC");
      return nullptr;
    }
    return makeUniqueNoThrow<XtcReaderDocument>(*this, std::move(xtc));
  }

  if (isTxtFile(path)) {
    auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
    if (!txt) {
      LOG_ERR("READER", "Failed to allocate TXT object");
      return nullptr;
    }
    if (!txt->load()) {
      LOG_ERR("READER", "Failed to load TXT");
      return nullptr;
    }
    return makeUniqueNoThrow<TxtReaderDocument>(*this, std::move(txt));
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }

  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    allowFastInitialRefresh = false;
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }

  if (!loaded) {
    LOG_ERR("READER", "Failed to load epub");
    return nullptr;
  }

  return makeUniqueNoThrow<EpubReaderDocument>(*this, std::move(epub));
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoBack() { finish(); }

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();
    return;
  }

  if (isImageFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  currentBookPath = initialBookPath;
  document = createDocument(initialBookPath);
  if (!document) {
    onGoBack();
    return;
  }

  if (!document->load(allowFastInitialRefresh)) {
    onGoBack();
    return;
  }

  APP_STATE.openEpubPath = document->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(document->getPath(), document->getTitle(), document->getAuthor(), document->getThumbBmpPath());

  requestUpdate();
}

void ReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
  document.reset();
}

void ReaderActivity::loop() {
  if (!document) {
    finish();
    return;
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, document->getPath().c_str(),
                                        {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  const bool atEndOfBook = document->isAtEndOfBook();

  if (!atEndOfBook && endOfBookOptionsReady.load(std::memory_order_acquire)) {
    RenderLock lock(*this);
    endOfBookOptionsReady.store(false, std::memory_order_release);
    endOfBookOptions.reset();
  }

  if (atEndOfBook && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
    std::string openPath;
    switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        document->onReturnFromEndOfBook();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  document->loop();

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (atEndOfBook) {
    if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      document->onReturnFromEndOfBook();
      requestUpdate();
    }
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool skipPages =
      !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP && heldMs > ReaderUtils::SKIP_HOLD_MS;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (skipPages) {
      document->skipPages(-skipAmount);
    } else {
      document->pageTurn(false);
    }
    requestUpdate();
  } else if (nextTriggered) {
    if (skipPages) {
      document->skipPages(skipAmount);
    } else {
      document->pageTurn(true);
    }
    requestUpdate();
  }
}

void ReaderActivity::render(RenderLock&& lock) {
  if (!document) {
    return;
  }

  if (document->isAtEndOfBook()) {
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
      endOfBookOptionsReady.store(endOfBookOptions != nullptr, std::memory_order_release);
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(document->getPath());
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    return;
  }

  ReaderRenderContext context{renderer, pagesUntilFullRefresh, forcedRefreshPending, lock};
  document->render(context);

  if (!document->rendersOwnStatusBar()) {
    document->renderStatusBar(renderer);
  }

  if (!document->commitsDisplayBuffer()) {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}

bool ReaderActivity::skipLoopDelay() {
  return document ? document->skipLoopDelay() : false;
}

bool ReaderActivity::appliesNightMode() const {
  return document ? document->appliesNightMode() : true;
}

bool ReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}

ScreenshotInfo ReaderActivity::getScreenshotInfo() const {
  return document ? document->getScreenshotInfo() : ScreenshotInfo{};
}
