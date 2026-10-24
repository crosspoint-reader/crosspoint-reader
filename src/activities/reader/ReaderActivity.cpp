#include "ReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Memory.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "XtcReaderActivity.h"
#include "util/PluginEvents.h"

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh)
    : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)) {
  if (allowFastInitialRefresh) {
    const int refreshFrequency = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = refreshFrequency > 1 ? refreshFrequency : 2;
  }
}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh,
                                                       const bool pluginDocument) {
  // ActivityManager requires heap ownership; each branch allocates exactly one screen-lifetime object.
  std::unique_ptr<ReaderActivity> activity;
  if (FsHelpers::hasXtcExtension(path)) {
    activity = makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else {
    activity = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (!activity) {
    LOG_ERR("READER", "OOM: reader activity");
    return activity;
  }
  activity->pluginDocument = pluginDocument;
  return activity;
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }

void ReaderActivity::notePageTurn(const bool forward, const bool succeeded) {
  RenderLock lock(*this);
  readerSession.noteTurn(forward, succeeded);
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  // Heap ledger for field crash reports: free vs largest block distinguishes a
  // leak (free falls) from fragmentation (free stable, largest collapses).
  LOG_INF("MEM", "reader enter: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  // Clear remembered book after opening it. A document leaves the user's book in place.
  if (!pluginDocument && !APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

  sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  if (!loadBook()) {
    if (!handleLoadFailure()) finish();
    return;
  }

  requestUpdate();
}

void ReaderActivity::rememberBookOnceRendered() {
  if (pluginDocument || bookRemembered || !pageRendered.load(std::memory_order_acquire)) return;
  bookRemembered = true;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
  const pluginevents::Var openVars[] = {{"book", bookPath.c_str()}};
  pluginevents::emit(pluginevents::Event::ReaderOpen, openVars, 1);
}

void ReaderActivity::onExit() {
  Activity::onExit();

  // Keep rebuildable font buffers from pinning the heap between reading sessions.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  LOG_INF("MEM", "reader exit: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // Flush BEFORE the ReaderExit event: the session's final progress must be
  // durable before a subscriber can act on the exit notification.
  flushReaderSession();

  if (!pluginDocument && pluginevents::anySubscriber(pluginevents::Event::ReaderExit)) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d", getScreenshotInfo().progressPercent);
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()}, {"percent", percent}};
    pluginevents::emit(pluginevents::Event::ReaderExit, vars, 2);
  }

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

void ReaderActivity::prepareForSleep() { flushReaderSession(); }

void ReaderActivity::flushReaderSession() {
  if (pluginDocument || !readerSession.isEmitWorthy() ||
      !pluginevents::anySubscriber(pluginevents::Event::ReaderSession)) {
    readerSession.reset();
    return;
  }

  const std::string document = KOReaderDocumentId::calculate(bookPath);
  const bool validDocument =
      document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
  if (validDocument) {
    char startTime[24];
    char endTime[24];
    char duration[16];
    char startProgress[8];
    char endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(readerSession.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld", static_cast<long long>(readerSession.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(readerSession.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", readerSession.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", readerSession.endProgressBp());
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()},       {"document", document.c_str()},
                                      {"start_time", startTime},        {"end_time", endTime},
                                      {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                                      {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
  }
  readerSession.reset();
}

bool ReaderActivity::handleBackNavigation() {
  if (pluginDocument) {
    // Same Back edges as below, but both lead back to the plugin menu.
    if (mappedInput.wasBackGesture()) return false;
    if (!mappedInput.wasLongPressed(MappedInputManager::Button::Back, ReaderUtils::GO_BACK_OR_HOME_MS) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      return false;
    }
    leaveReader();
    return true;
  }
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, bookPath.c_str(),
                                           {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::endOfBookMenuActive() const {
  return isAtEndOfBook() && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (suppressConfirmRelease || !endOfBookMenuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      leaveReader();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }

  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;

  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
    return true;
  }
  if (nextTriggered) {
    leaveReader();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

// Boards without a Back button (or with touch off) leave through Home: a document
// still lands on its plugin menu, and Home from there goes Home.
bool ReaderActivity::handleHomeGesture() {
  if (!pluginDocument) return false;
  leaveReader();
  return true;
}

void ReaderActivity::leaveReader() {
  // Back and Home both lead here for a document, so a failed menu must not
  // keep the reader on screen.
  if (pluginDocument && activityManager.goToPluginMenuFor(bookPath)) return;
  onGoHome();
}

void ReaderActivity::loop() {
  rememberBookOnceRendered();
  clearEndOfBookOptionsIfNeeded();
  if (handleEndOfBookMenu()) return;
  if (handleFormatInput()) return;
  if (handleBackNavigation()) return;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) return;
  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool skip =
      !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP && heldMs >= ReaderUtils::SKIP_HOLD_MS;

  const bool changed = skip ? skipPages(prevTriggered ? -10 : 10) : pageTurn(!prevTriggered);
  // A skip is navigation, not reading: it never counts toward session dwell.
  notePageTurn(!skip && !prevTriggered, changed);
  if (changed && (touch.prev || touch.next)) haptic_feedback::touchAction(skip);
  requestUpdate();
}

void ReaderActivity::render(RenderLock&&) {
  if (isAtEndOfBook()) {
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      // A document suggests no sibling files: they would open as books, outside the plugin's menu.
      endOfBookOptions->loadOnce(bookPath, !pluginDocument);
      // Release-publish AFTER loadOnce() so the main task's acquire load can't
      // observe an object whose names/selector are still being populated.
      endOfBookOptionsReady.store(true, std::memory_order_release);
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    onEndOfBookRendered();
    markPageRendered();
    readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
    return;
  }

  renderBook();
  readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
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
