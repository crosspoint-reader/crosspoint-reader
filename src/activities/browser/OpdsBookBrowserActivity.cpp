#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsAuthDoc.h>
#include <OpdsFeedParser.h>
#include <OpdsSearchTemplate.h>
#include <OpenSearchDescParser.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
// OPDS authentication documents are small; cap the 401-body capture.
constexpr size_t MAX_AUTH_DOC_BYTES = 8192;

// Percent-encode a value for a query string or form-urlencoded body.
std::string percentEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

}  // namespace

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : Activity("OpdsBookBrowser", renderer, mappedInput),
      UiAppHost(renderer),
      buttonNavigator(),
      server(std::move(server)) {}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  searchDescriptionUrl = "";
  searchTemplateBase = "";
  bearerToken = "";
  currentPath = "";
  searchQuery.clear();
  headerSearchTitle.clear();
  searchQueryHistory.clear();
  pageNextHref.clear();
  pagePrevHref.clear();
  pageFirstHref.clear();
  pageLastHref.clear();
  feedTitle.clear();
  pageLabel[0] = '\0';
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::activateSelected() {
  if (entries.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[selectorIndex];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->entries.size())) return;
  self->selectorIndex = event.value;
  // The tapped row leaves the screen either way (new feed or download view);
  // a lingering tap flash would gray an unrelated row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void OpdsBookBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void OpdsBookBrowserActivity::setSearchQuery(const std::string& query) {
  searchQuery = query;
  headerSearchTitle = query.empty() ? std::string() : "\u201c" + query + "\u201d";
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (hasSearch() && selectorIndex == 0) launchSearch();
    }

    // Touch goes through the FreeInkApp: render() registered every tap target
    // (rows, header search button); route the snapshot and let the registered
    // handlers dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed) {
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (route) return;  // dispatched to onRowEvent/onSearchEvent
      if (state != BrowserState::BROWSING) return;
    }

    if (!entries.empty()) {
      // Swipes scroll the viewport; the selection stays put (it may scroll
      // off-screen) and button navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.visibleRows : -listNav.visibleRows;
        if (listNav.scrollBy(delta, static_cast<int>(entries.size()))) requestUpdate();
        return;
      }

      const auto moveSelection = [this](const int index) {
        selectorIndex = index;
        listNav.selected = index;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, entries.size())); });
      buttonNavigator.onPreviousRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, entries.size())); });
      buttonNavigator.onNextContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), listNav.visibleRows));
      });
      buttonNavigator.onPreviousContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), listNav.visibleRows));
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void OpdsBookBrowserActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

// Shared chrome for every state: reserve the firmware's button-hint band and
// draw the themed header (padding, centering, and rule come from the theme).
void OpdsBookBrowserActivity::screenHeader(UiScreen& screen, const bool withSearch) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  // Same top offset as every GUI.drawHeader caller, so the band lines up with
  // the rest of the firmware's screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().topPadding));
  fui::HeaderProps header;
  // An active search replaces the server name with the quoted query, like the
  // library view, so the reader can see what produced the current list. With
  // no search, a navigated feed's own title beats the server name.
  header.title = !headerSearchTitle.empty() ? headerSearchTitle.c_str()
                 : !feedTitle.empty()       ? feedTitle.c_str()
                 : server.name.empty()      ? tr(STR_OPDS_BROWSER)
                                            : server.name.c_str();
  if (state == BrowserState::BROWSING && pageLabel[0] != '\0') header.subtitle = pageLabel;
  header.borderEdges = fui::EdgeBottom;
  if (withSearch && hasSearch()) {
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    // Optically align the icon with the title glyphs: text hangs low in its
    // line cell by the font's internal leading; drop the button to match.
    const int titleFontId = uiScaleSpec().titleFontId;
    header.actionOffsetY =
        static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  }
  screen.header(header);
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  screenHeader(screen, true);

  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Transient per-render: sized once via reserve, points into `entries`
  // strings, freed on scope exit.
  // rowItems is built whenever entries changes (see rebuildRowItems(), called
  // from fetchFeed()/releaseEntries()) and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  listNav.selected = selectorIndex;
  props.partialTrailingRow = true;
  screen.syncListViewport(listNav, props, static_cast<int>(entries.size()));
  screen.list(props);
}

void OpdsBookBrowserActivity::buildDownloadScreen(UiScreen& screen) {
  screenHeader(screen, false);

  // Centered block: status line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsBookBrowserActivity::buildStatusScreen(UiScreen& screen) {
  screenHeader(screen, false);

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  // CHECK_WIFI / LOADING (and the brief child-activity handoff states).
  screen.centeredText(statusMessage.c_str(), centered);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::BROWSING: {
      const char* confirmLabel =
          (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
      const char* searchLabel = (hasSearch() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderUi();
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsFeedParser parser;
  for (int authAttempt = 0;; ++authAttempt) {
    int status = 0;
    HttpDownloader::FetchOptions options;
    options.username = server.username;
    options.password = server.password;
    options.bearer = bearerToken;
    // Prefer OPDS 2.0 JSON from servers that content-negotiate (e.g.
    // Mayberry); Atom-only servers ignore this and serve their usual feed.
    options.accept = "application/opds+json,application/atom+xml;q=0.9,*/*;q=0.8";
    options.statusOut = &status;
    const bool fetched = HttpDownloader::fetchUrl(
        url,
        [&parser](const uint8_t* data, const size_t len) {
          parser.write(data, len);
          return !parser.error();  // abort the transfer on a parse error
        },
        options);
    parser.flush();

    if (!fetched && status == 401) {
      // Authentication for OPDS: the 401 body is an authentication document
      // describing the server's flows. One re-auth attempt (covers both a
      // missing and an expired token), then give up.
      bearerToken.clear();
      if (authAttempt == 0 && authenticateWithServer(url)) {
        parser.reset();  // drop any finalized backend before the retry
        continue;
      }
      state = BrowserState::ERROR;
      errorMessage = tr(STR_OPDS_AUTH_FAILED);
      requestUpdate();
      return;
    }
    if (parser.error()) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_PARSE_FEED_FAILED);
      requestUpdate();
      return;
    }
    if (!fetched) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
    break;
  }

  searchTemplate = parser.getSearchTemplate();
  searchDescriptionUrl = parser.getSearchDescriptionUrl();
  searchTemplateBase = "";  // feed-inline template resolves against the feed URL
  feedTitle = parser.getFeedTitle();

  // "Page X of Y" from the feed's pagination metadata (OPDS 2.0 metadata
  // object or the opensearch:* elements of an Atom feed).
  pageLabel[0] = '\0';
  const uint32_t itemsPerPage = parser.getItemsPerPage();
  const uint32_t totalItems = parser.getNumberOfItems();
  const uint32_t page = parser.getCurrentPage();
  const uint32_t totalPages = itemsPerPage > 0 ? (totalItems + itemsPerPage - 1) / itemsPerPage : 0;
  if (page > 0 && totalPages > 1) {
    snprintf(pageLabel, sizeof(pageLabel), tr(STR_OPDS_PAGE_POSITION), static_cast<unsigned long>(page),
             static_cast<unsigned long>(totalPages));
  }
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  pageNextHref = nextUrl;
  pagePrevHref = prevUrl;
  // First/Last rows only when they reach further than Previous/Next.
  pageFirstHref = (!parser.getFirstPageUrl().empty() && !prevUrl.empty() && parser.getFirstPageUrl() != prevUrl)
                      ? parser.getFirstPageUrl()
                      : "";
  pageLastHref = (!parser.getLastPageUrl().empty() && !nextUrl.empty() && parser.getLastPageUrl() != nextUrl)
                     ? parser.getLastPageUrl()
                     : "";
  const bool feedTruncated = parser.truncated();
  // Reset the selection before the swap: the render task reads
  // entries[selectorIndex] under only an empty() guard, and the new feed can
  // be shorter than the old selection.
  selectorIndex = 0;
  listNav.reset();
  entries = parser.takeEntries();

  auto facetRows = parser.takeFacetEntries();
  entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1) +
                  (pageFirstHref.empty() ? 0 : 1) + (pageLastHref.empty() ? 0 : 1) + facetRows.size());
  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!pageFirstHref.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_FIRST_PAGE), "", pageFirstHref, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }
  if (!pageLastHref.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_LAST_PAGE), "", pageLastHref, ""});
  }
  // Facet groups (sort orders, filters) come after the catalog content, each
  // under its own section heading.
  for (auto& facet : facetRows) {
    entries.push_back(std::move(facet));
  }
  if (feedTruncated) {
    LOG_INF("OPDS", "Feed truncated to fit memory");
  }

  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  rebuildRowItems();
  requestUpdate();
}

// Derives rowItems from entries. Called whenever entries changes
// (fetchFeed()/releaseEntries()) so buildBrowsingScreen() reuses the cached
// rows on every repaint instead of rebuilding them per render.
void OpdsBookBrowserActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(entries.size());
  for (const auto& entry : entries) {
    fui::ListItem item;
    // A group's "see all" self link carries no title of its own; the UI
    // supplies the label (the group name is the section heading above it).
    item.label = entry.id == OPDS_SEE_ALL_ID ? tr(STR_OPDS_SEE_ALL) : entry.title.c_str();
    if (!entry.heading.empty()) item.sectionHeading = entry.heading.c_str();
    if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) item.subtitle = entry.author.c_str();
    if (entry.type == OpdsEntryType::NAVIGATION) item.value = entry.detail.empty() ? ">" : entry.detail.c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void OpdsBookBrowserActivity::releaseEntries() {
  // The app's interaction table holds row indices (and hit rects) for the old
  // entries; stop routing touches against it until the next render.
  closeRouting();
  std::vector<OpdsEntry>().swap(entries);
  std::vector<fui::ListItem>().swap(rowItems);
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  searchQueryHistory.push_back(searchQuery);
  // Following a results page (first/previous/next/last) stays within the
  // same search; any other navigation leaves it.
  if (!isPaginationHref(entry.href)) setSearchQuery("");
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  releaseEntries();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    if (!searchQueryHistory.empty()) {
      setSearchQuery(searchQueryHistory.back());
      searchQueryHistory.pop_back();
    }
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    releaseEntries();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  // opdsDownloadFolder is already a null-terminated char[64]; use it directly —
  // no std::string copy. exists()/mkdir() take const char*.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // downloadToFile() needs a std::string, and titles are unbounded (a fixed
  // char[] would truncate). Cold path (a multi-second download follows), so one
  // reserve'd, in-place-appended owning string is the right call.
  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  // The selected book data is now copied into the download URL, filename, and
  // status line. Reclaim the catalog while TLS owns its record buffers; reload
  // the current feed when the transfer finishes.
  releaseEntries();

  // Rebuildable SD-font caches can hold tens of KB the TLS session needs for
  // a multi-MB book; release them up front (they repopulate on demand) and
  // refuse to start below the floor — a doomed transfer otherwise dies
  // mid-stream with MEMORY_E, or abort()s on an interior allocation.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
  LOG_DBG("OPDS", "Download heap: %u free, %u max block", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("OPDS", "Low heap for download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        // The activity loop is blocked for the whole download; pump input here
        // so the Cancel button or a Back press can abort mid-transfer.
        mappedInput.update(true);
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
        // Home cancels immediately; other configured actions are deferred to
        // the next main-loop pass by the transfer input pump.
        if (mappedInput.wasHomeGesture()) {
          cancelDownload = true;
          goHomeAfterCancel = true;
        }
        routeTouch(mappedInput);
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelDownload, server.username, server.password, false, bearerToken);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    fetchFeed(currentPath);
    return;
  } else if (result == HttpDownloader::ABORTED) {
    // The partial file is already removed. Reload the released catalog unless
    // the cancel came from the home gesture.
    LOG_INF("OPDS", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    fetchFeed(currentPath);
    return;
  } else {
    LOG_ERR("OPDS", "Download failed: %d", static_cast<int>(result));
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), searchQuery);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

// Authentication for OPDS 1.0: re-request the resource capturing the 401 body
// (the authentication document), pick a flow the device can drive, and obtain
// credentials for retrying.
//  - basic: already sent preemptively when credentials are stored, so landing
//    here means they are missing or wrong.
//  - oauth/password: POST the stored credentials to the "authenticate" link
//    and keep the returned access token as a Bearer header.
//  - oauth/implicit: needs a browser; cannot be driven from the device.
bool OpdsBookBrowserActivity::authenticateWithServer(const std::string& resourceUrl) {
  std::string body;
  body.reserve(1024);
  int status = 0;
  HttpDownloader::FetchOptions options;
  options.statusOut = &status;
  options.captureErrorBody = true;
  HttpDownloader::fetchUrl(
      resourceUrl,
      [&body](const uint8_t* data, const size_t len) {
        const size_t room = MAX_AUTH_DOC_BYTES - body.size();
        body.append(reinterpret_cast<const char*>(data), len < room ? len : room);
        return true;
      },
      options);
  if (status != 401 || body.empty()) return false;

  OpdsAuthDoc doc;
  if (!parseOpdsAuthDocument(body.data(), body.size(), doc)) {
    LOG_ERR("OPDS", "401 without a usable authentication document");
    return false;
  }

  if (doc.hasOauthPassword && !doc.tokenUrl.empty() && !server.username.empty() && !server.password.empty()) {
    const std::string tokenUrl = UrlUtils::buildUrl(resourceUrl, doc.tokenUrl);
    const std::string form = "grant_type=password&username=" + percentEncode(server.username) +
                             "&password=" + percentEncode(server.password);
    std::string response;
    int tokenStatus = 0;
    if (HttpDownloader::postForm(tokenUrl, form, response, &tokenStatus)) {
      std::string token;
      if (extractJsonStringField(response.data(), response.size(), "access_token", token) && !token.empty()) {
        bearerToken = std::move(token);
        LOG_INF("OPDS", "OAuth password grant succeeded");
        return true;
      }
    }
    LOG_ERR("OPDS", "OAuth token request failed (status %d)", tokenStatus);
    return false;
  }

  if (doc.hasOauthImplicit && !doc.hasBasic && !doc.hasOauthPassword) {
    LOG_ERR("OPDS", "Server only offers browser-based OAuth (implicit)");
  }
  return false;
}

// Resolves the search template lazily: OPDS 1.x servers such as calibre-web,
// COPS and Kavita publish it in a separate OpenSearch description document
// instead of inlining it in the feed.
bool OpdsBookBrowserActivity::ensureSearchTemplate() {
  if (!searchTemplate.empty()) return true;
  if (searchDescriptionUrl.empty()) return false;

  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  const std::string descUrl = UrlUtils::buildUrl(feedUrl, searchDescriptionUrl);
  LOG_DBG("OPDS", "Fetching OpenSearch description: %s", descUrl.c_str());
  OpenSearchDescParser parser;
  HttpDownloader::FetchOptions options;
  options.username = server.username;
  options.password = server.password;
  options.bearer = bearerToken;
  const bool fetched = HttpDownloader::fetchUrl(
      descUrl,
      [&parser](const uint8_t* data, const size_t len) {
        parser.write(data, len);
        return !parser.error();
      },
      options);
  parser.flush();
  if (!fetched || parser.error() || parser.getTemplate().empty()) {
    LOG_ERR("OPDS", "OpenSearch description unusable");
    return false;
  }
  searchTemplate = parser.getTemplate();
  searchTemplateBase = descUrl;  // relative templates resolve against the description doc
  return true;
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate();

  if (!ensureSearchTemplate()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  // Expand the template first: buildUrl percent-encodes braces, so a raw
  // template must never pass through URL resolution.
  const std::string expanded = expandOpdsSearchTemplate(searchTemplate, percentEncode(query));
  const std::string base =
      searchTemplateBase.empty() ? UrlUtils::buildUrl(server.url, currentPath) : searchTemplateBase;
  const std::string url = UrlUtils::buildUrl(base, expanded);

  navigationHistory.push_back(currentPath);
  searchQueryHistory.push_back(searchQuery);
  setSearchQuery(query);
  currentPath = url;

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  releaseEntries();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
