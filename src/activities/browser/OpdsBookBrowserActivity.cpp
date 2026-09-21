#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsFeedParser.h>
#include <OpdsPublicationDoc.h>
#include <OpdsSearchTemplate.h>
#include <WiFi.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsTokenStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/opdsIcons.h"
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
constexpr fui::ActionId ACTION_PAGE = 4;
constexpr fui::ActionId ACTION_DETAIL = 5;
// ACTION_PAGE values: which pagination link a tab follows.
constexpr int16_t PAGE_FIRST = 0;
constexpr int16_t PAGE_PREV = 1;
constexpr int16_t PAGE_NEXT = 2;
constexpr int16_t PAGE_LAST = 3;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

// Accept-Language from the reader's UI language, so servers that localize the
// catalog (e.g. Lirtuel) return it translated. Primary subtag plus an English
// fallback; derived from the exposed LANGUAGE_CODES table (e.g. "FR" -> "fr").
std::string uiAcceptLanguage() {
  const char* code = LANGUAGE_CODES[static_cast<int>(SETTINGS.language)];
  std::string lang;
  for (const char* p = code; *p && *p != '_'; ++p) {
    lang += (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p + 32) : *p;
  }
  if (lang.empty() || lang == "en") return "en";
  return lang + ",en;q=0.8";
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
  // Configure the OPDS client for this server, restore any persisted token, and
  // reset the per-session auth latches. Accept-Language localizes the catalog
  // (both request header and language-map title resolution).
  opdsClient.setServer(server.url, server.username, server.password);
  {
    const OpdsTokens t = OPDS_TOKENS.get(tokenKey());
    opdsClient.setTokens(t.accessToken, t.refreshToken, t.refreshUrl);
  }
  opdsClient.resetAuthState();
  opdsClient.setAcceptLanguage(uiAcceptLanguage());
  opdsClient.onStatus(&OpdsBookBrowserActivity::onClientStatus, this);
  currentPath = "";
  searchQuery.clear();
  headerSearchTitle.clear();
  searchQueryHistory.clear();
  pageNextHref.clear();
  pagePrevHref.clear();
  pageFirstHref.clear();
  pageLastHref.clear();
  feedTitle.clear();
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.on(ACTION_PAGE, &OpdsBookBrowserActivity::onPageEvent, this);
  app.on(ACTION_DETAIL, &OpdsBookBrowserActivity::onDetailEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();
  if (!detailCoverPath.empty()) {
    if (Storage.exists(detailCoverPath.c_str())) Storage.remove(detailCoverPath.c_str());
    detailCoverPath.clear();
  }
  detailCoverReady = false;

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::activateSelected() {
  if (entries.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[selectorIndex];
  entry.type == OpdsEntryType::BOOK ? openPublicationDetail(entry) : navigateToEntry(entry);
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

void OpdsBookBrowserActivity::onPageEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  const std::string* href = nullptr;
  switch (event.value) {
    case PAGE_FIRST:
      href = &self->pageFirstHref;
      break;
    case PAGE_PREV:
      href = &self->pagePrevHref;
      break;
    case PAGE_NEXT:
      href = &self->pageNextHref;
      break;
    case PAGE_LAST:
      href = &self->pageLastHref;
      break;
    default:
      return;
  }
  if (href && !href->empty()) self->followPageLink(*href);
}

// Follow a pagination link. isPaginationHref() keeps the search-term header
// across page turns; navigateToEntry() does the history push and fetch.
void OpdsBookBrowserActivity::followPageLink(const std::string& href) {
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.href = href;
  navigateToEntry(entry);
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

  if (state == BrowserState::DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Entries were released for heap; rebuild the catalog we came from.
      releaseEntries();
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate();
      fetchFeed(currentPath);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      downloadBook(detailBook);
      return;
    }
    // The publication page composes to fit (long descriptions are truncated by
    // priority), so there is nothing to scroll; only the acquire button routes.
    const auto route = routeTouch(mappedInput);
    if (route.routed) {
      if (app.invalidated()) requestUpdate();
      if (route) return;  // dispatched to onDetailEvent (the action button)
      if (state != BrowserState::DETAIL) return;
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    // Side page-turn buttons follow the feed's pagination links, the natural
    // e-reader mapping now that the Next/Previous rows are a touch tab bar.
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) && !pageNextHref.empty()) {
      followPageLink(pageNextHref);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) && !pagePrevHref.empty()) {
      followPageLink(pagePrevHref);
      return;
    }

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
        // Step by the rows the last build actually drew, not the fixed-height
        // visibleRows estimate: OPDS rows vary in height (author subtitles,
        // section headings), so the estimate overshoots and would skip past a
        // partially-shown trailing row. requestScroll defers the clamp to the
        // render task's syncToProps (never touch render state from here).
        const int delta =
            swipe == MappedInputManager::SwipeDir::Up ? listNav.inputPageRows() : -listNav.inputPageRows();
        listNav.requestScroll(delta);
        requestUpdate();
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
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), listNav.inputPageRows()));
      });
      buttonNavigator.onPreviousContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), listNav.inputPageRows()));
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
    case BrowserState::DETAIL:
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
    case BrowserState::DETAIL:
      self->buildDetailScreen(screen);
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

// Bottom pagination bar: arrow-icon tabs following the feed's first/prev/next/
// last links. Prev and Next always show when the feed is paginated (the
// unavailable direction is disabled); First/Last appear only when advertised.
void OpdsBookBrowserActivity::buildPaginationBar(UiScreen& screen) {
  constexpr int MAX_PAGE_TABS = 4;
  fui::TabItem tabs[MAX_PAGE_TABS];
  int count = 0;
  const auto addTab = [&](const freeink::Icon& icon, const int16_t value, const std::string& href) {
    if (count >= MAX_PAGE_TABS) return;
    tabs[count].icon = fui::bitmapFromIcon(icon);
    tabs[count].value = value;
    tabs[count].enabled = !href.empty();
    ++count;
  };
  if (!pageFirstHref.empty()) addTab(icon_page_first_32, PAGE_FIRST, pageFirstHref);
  addTab(icon_page_prev_32, PAGE_PREV, pagePrevHref);
  addTab(icon_page_next_32, PAGE_NEXT, pageNextHref);
  if (!pageLastHref.empty()) addTab(icon_page_last_32, PAGE_LAST, pageLastHref);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect band = screen.takeBottom(static_cast<int16_t>(metrics.tabBarHeight));
  // Full-width band with a top rule, matching the tab chrome elsewhere.
  const fui::Rect frameRect = screen.frame().screen();
  const fui::Rect barRect{frameRect.x, band.y, frameRect.width, band.height};
  screen.target().fill(fui::Rect{barRect.x, barRect.y, barRect.width, 1}, fui::Paint::solid(fui::Color::Black));

  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_PAGE;
  props.inputMask = fui::InputTouch;
  props.iconSize = 32;
  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const fui::Rect slots{static_cast<int16_t>(barRect.x + side), static_cast<int16_t>(barRect.y + 1),
                        static_cast<int16_t>(barRect.width - 2 * side), static_cast<int16_t>(barRect.height - 1)};
  fui::tabBar(screen.frame(), slots, props);
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  screenHeader(screen, true);

  // Reserve the pagination band before the list claims the remaining height.
  if (hasPagination()) buildPaginationBar(screen);

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
      const char* confirmLabel = tr(STR_OPEN);
      if (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) {
        confirmLabel = entries[selectorIndex].purchase ? tr(STR_OPDS_BUY) : tr(STR_DOWNLOAD);
      }
      const char* searchLabel = (hasSearch() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DETAIL: {
      labels = mappedInput.mapLabels(tr(STR_BACK), acquireLabel(), "", "");
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
  // Decode the book cover into the rect the layout reserved. Done here, at the
  // top of the render task, rather than inside the component call chain, so the
  // JPEG decoder's stack cost doesn't stack on the deep FreeInkUI compose path.
  if (state == BrowserState::DETAIL && detailCoverReady && detailCoverRect.width > 0 && detailCoverRect.height > 0) {
    if (ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(detailCoverPath)) {
      RenderConfig cfg{detailCoverRect.x, detailCoverRect.y, detailCoverRect.width, detailCoverRect.height};
      decoder->decodeToFramebuffer(detailCoverPath, renderer, cfg);
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  const std::string url = UrlUtils::buildUrl(server.url, path);
  OpdsFeedParser parser;
  const auto status = opdsClient.fetchFeed(url, parser);
  // The token state may have changed (renewed, or cleared on failure); persist
  // only when an auth handshake actually ran.
  if (opdsClient.tokensDirty()) persistTokens();
  if (status != freeink::opds::OpdsClient::FetchStatus::Ok) {
    state = BrowserState::ERROR;
    switch (status) {
      case freeink::opds::OpdsClient::FetchStatus::CredentialsMissing:
        errorMessage = tr(STR_SET_CREDENTIALS_FIRST);
        break;
      case freeink::opds::OpdsClient::FetchStatus::AuthFailed:
        errorMessage = tr(STR_OPDS_AUTH_FAILED);
        break;
      case freeink::opds::OpdsClient::FetchStatus::ParseFailed:
        errorMessage = tr(STR_PARSE_FEED_FAILED);
        break;
      default:
        errorMessage = tr(STR_FETCH_FEED_FAILED);
        break;
    }
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  searchDescriptionUrl = parser.getSearchDescriptionUrl();
  searchTemplateBase = "";  // feed-inline template resolves against the feed URL
  feedTitle = parser.getFeedTitle();

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

  // Pagination is a bottom tab bar (buildPaginationBar), not list rows.
  auto facetRows = parser.takeFacetEntries();
  entries.reserve(entries.size() + facetRows.size());
  // Facet groups (sort orders, filters) come after the catalog content, each
  // under its own section heading.
  entries.insert(entries.end(), std::make_move_iterator(facetRows.begin()), std::make_move_iterator(facetRows.end()));

  // Standard OPDS user collections (loans, reading list, history), advertised
  // as feed-level links, become navigation rows under a "My Account" heading
  // so they are reachable without pasting a URL. Authentication kicks in when
  // one is opened. Only surfaced on the catalog root (no navigation history),
  // since the same links repeat on every feed.
  if (navigationHistory.empty()) {
    struct UserLink {
      const std::string& href;
      const char* label;  // tr()'d value; runtime StrId can't go through the macro
    };
    const UserLink userLinks[] = {
        {parser.getShelfUrl(), tr(STR_OPDS_SHELF)},
        {parser.getWishlistUrl(), tr(STR_OPDS_WISHLIST)},
        {parser.getHistoryUrl(), tr(STR_OPDS_HISTORY)},
    };
    bool firstUserLink = true;
    for (const auto& link : userLinks) {
      if (link.href.empty() || entries.size() >= OpdsLimits::MAX_ENTRIES) continue;
      OpdsEntry entry;
      entry.type = OpdsEntryType::NAVIGATION;
      entry.title = link.label;
      entry.href = link.href;
      if (firstUserLink) {
        entry.heading = tr(STR_OPDS_MY_ACCOUNT);
        firstUserLink = false;
      }
      entries.push_back(std::move(entry));
    }
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
    // Purchases show their price in the value column.
    if (entry.type == OpdsEntryType::BOOK && entry.purchase && !entry.detail.empty()) item.value = entry.detail.c_str();
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

  // Indirect acquisition (OPDS 2.0 5.3): the link points at a publication
  // document, not the EPUB. The client fetches it and resolves the real
  // download link. Library loans behind this are usually LCP-encrypted, which
  // the reader can't open; the post-download EPUB check reports that cleanly.
  if (book.indirect) {
    std::string resolved;
    bool resolvedEpub = false;
    if (!opdsClient.resolveIndirect(downloadUrl, resolved, resolvedEpub)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_OPDS_NOT_A_BOOK);
      requestUpdate();
      return;
    }
    downloadUrl = resolved;
  }
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
  const freeink::opds::HttpAuth dlAuth = opdsClient.downloadAuth();
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
      &cancelDownload, dlAuth.username, dlAuth.password, false, dlAuth.bearer);

  if (result == HttpDownloader::OK) {
    // A purchase link (or any misbehaving endpoint) can answer 200 with an
    // HTML page; a real EPUB is a ZIP container. Check the magic before
    // accepting the file.
    uint8_t magic[4] = {0};
    {
      HalFile check;
      if (Storage.openFileForRead("OPDS", filename.c_str(), check)) {
        check.read(magic, sizeof(magic));
      }
      if (check.isOpen()) check.close();  // close before any remove() on the same path
    }
    if (!(magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4)) {
      LOG_ERR("OPDS", "Downloaded file is not an EPUB (magic %02x%02x%02x%02x)", magic[0], magic[1], magic[2],
              magic[3]);
      Storage.remove(filename.c_str());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_OPDS_NOT_A_BOOK);
      requestUpdate();
      return;
    }
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

void OpdsBookBrowserActivity::onDetailEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DETAIL) return;
  self->app.clearTapFlash();
  self->downloadBook(self->detailBook);
}

// Open the publication detail page: fetch the publication's self-document for
// full metadata + availability, then show it with an acquire button. The
// catalog list and SD font caches are released first (rebuilt on Back) to keep
// the most heap free for the TLS fetch, exactly like a download.
void OpdsBookBrowserActivity::openPublicationDetail(const OpdsEntry& entry) {
  detailBook = entry;
  currentPublication = OpdsPublication{};

  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  const bool haveSelf = !entry.selfHref.empty();
  const std::string docUrl = UrlUtils::buildUrl(feedUrl, haveSelf ? entry.selfHref : entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);

  releaseEntries();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();

  if (haveSelf) {
    opdsClient.fetchPublication(docUrl, currentPublication);
  }

  if (!currentPublication.valid) {
    // No self link, or the fetch/parse failed: show what the list row had.
    currentPublication = OpdsPublication{};
    currentPublication.title = entry.title;
    currentPublication.author = entry.author;
    currentPublication.acquisitionHref = entry.href;
    currentPublication.indirect = entry.indirect;
    currentPublication.purchase = entry.purchase;
    if (entry.purchase) currentPublication.price = entry.detail;
    currentPublication.valid = !entry.title.empty();
  }

  // Resolve the acquisition to an absolute URL against the document it came
  // from; downloadBook() resolves against the feed, so an absolute URL is used
  // verbatim.
  if (!currentPublication.acquisitionHref.empty()) {
    detailBook.href = UrlUtils::buildUrl(docUrl, currentPublication.acquisitionHref);
    detailBook.indirect = currentPublication.indirect;
    detailBook.purchase = currentPublication.purchase;
  }

  // Fetch the cover art (best-effort) while entries and font caches are still
  // released, so the TLS transfer has maximum heap. Resolved against the
  // document the cover href came from.
  loadDetailCover(haveSelf ? docUrl : feedUrl);

  rebuildDetailInfo();
  state = BrowserState::DETAIL;
  requestUpdate();
}

// Downloads the publication's cover art to an SD temp file for the detail page.
// A cover is optional chrome: any failure just leaves the placeholder, never
// blocks the page.
void OpdsBookBrowserActivity::loadDetailCover(const std::string& docUrl) {
  detailCoverReady = false;
  detailCoverRect = fui::Rect{};
  if (!detailCoverPath.empty()) {
    if (Storage.exists(detailCoverPath.c_str())) Storage.remove(detailCoverPath.c_str());
    detailCoverPath.clear();
  }
  if (currentPublication.coverHref.empty()) return;

  const std::string url = UrlUtils::buildUrl(docUrl, currentPublication.coverHref);
  // The decoder factory picks JPEG vs PNG by file extension; name the temp to
  // match. Default to JPEG (the common case, and content-negotiated URLs with
  // no extension).
  std::string lower;
  lower.reserve(url.size());
  for (const char c : url) lower += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
  const bool isPng = lower.find(".png") != std::string::npos;
  std::string tmp = "/.crosspoint/opds_cover";
  tmp += isPng ? ".png" : ".jpg";

  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP) {
    LOG_INF("OPDS", "Skipping cover: low heap");
    return;
  }
  const freeink::opds::HttpAuth auth = opdsClient.downloadAuth();
  const auto result =
      HttpDownloader::downloadToFile(url, tmp, nullptr, nullptr, auth.username, auth.password, false, auth.bearer);
  if (result != HttpDownloader::OK) {
    LOG_ERR("OPDS", "Cover download failed: %d", static_cast<int>(result));
    if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
    return;
  }
  detailCoverPath = std::move(tmp);
  detailCoverReady = true;
}

bool OpdsBookBrowserActivity::detailCoverPainter(fui::DrawTarget&, fui::Rect rect, const fui::PublicationHeaderProps&,
                                                 void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  // Record the rect only; the decode runs at the end of render() to keep the
  // JPEG decoder off this deep component call chain.
  self->detailCoverRect = rect;
  return true;
}

// Formats the availability and metadata lines the publication-page component
// draws. Owned by the activity so the component's borrowed const char* stay
// valid across repaints; rebuilt once per opened publication.
void OpdsBookBrowserActivity::rebuildDetailInfo() {
  detailStatus.clear();
  detailCopies.clear();
  detailHolds.clear();
  detailMetadata.clear();

  const OpdsAvailability& av = currentPublication.availability;
  if (av.present()) {
    const char* stateStr = av.state == "available"     ? tr(STR_OPDS_AVAILABLE)
                           : av.state == "ready"       ? tr(STR_OPDS_READY)
                           : av.state == "reserved"    ? tr(STR_OPDS_RESERVED)
                           : av.state == "unavailable" ? tr(STR_OPDS_UNAVAILABLE)
                                                       : nullptr;
    if (stateStr) detailStatus = stateStr;
    if (av.copiesTotal >= 0) {
      char b[64];
      snprintf(b, sizeof(b), tr(STR_OPDS_COPIES), av.copiesAvailable < 0 ? 0 : av.copiesAvailable, av.copiesTotal);
      detailCopies = b;
    }
    // The reader's own queue position is more useful than the raw holds count.
    if (av.holdsPosition >= 0) {
      char b[48];
      snprintf(b, sizeof(b), tr(STR_OPDS_HOLD_POSITION), av.holdsPosition);
      detailHolds = b;
    } else if (av.holdsTotal >= 0) {
      char b[48];
      snprintf(b, sizeof(b), tr(STR_OPDS_HOLDS), av.holdsTotal);
      detailHolds = b;
    }
  }

  if (!currentPublication.purchase && !currentPublication.price.empty()) {
    detailMetadata = currentPublication.price;
  } else if (!currentPublication.publisher.empty()) {
    detailMetadata = currentPublication.publisher;
  }
}

const char* OpdsBookBrowserActivity::acquireLabel() const {
  if (detailBook.purchase) return tr(STR_OPDS_BUY);
  if (!detailBook.indirect) return tr(STR_DOWNLOAD);
  // Borrowable (library loan): offer to place a hold when no copy is free.
  const OpdsAvailability& av = currentPublication.availability;
  const bool noCopies = av.state == "unavailable" || (av.copiesAvailable == 0 && av.copiesTotal > 0);
  return noCopies ? tr(STR_OPDS_PLACE_HOLD) : tr(STR_OPDS_BORROW);
}

void OpdsBookBrowserActivity::buildDetailScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // No header: the publication component leads with the book's own title and
  // cover, so a separate title bar adds nothing. Reserve only the hardware
  // button-hint band so the acquire button clears it.
  screen.takeBottom(static_cast<int16_t>(metrics.buttonHintsHeight));

  const auto& theme = screen.theme();
  const auto asPtr = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };

  fui::PublicationPageProps props;
  props.book.title = asPtr(currentPublication.title);
  props.book.author = asPtr(currentPublication.author);
  // When a cover was downloaded, the painter reserves its rect and render()
  // decodes into it; otherwise the component draws its typeset placeholder.
  if (detailCoverReady) {
    props.book.coverPainter = &OpdsBookBrowserActivity::detailCoverPainter;
    props.book.coverPainterUserData = this;
  }
  props.book.titleText = theme.bodyText;
  props.book.detailText = theme.bodyText;
  props.availability.status = asPtr(detailStatus);
  props.availability.copies = asPtr(detailCopies);
  props.availability.holds = asPtr(detailHolds);
  props.availability.headingText = theme.bodyText;
  props.availability.detailText = theme.bodyText;
  props.descriptionHeading = tr(STR_OPDS_ABOUT_BOOK);
  props.description = asPtr(currentPublication.description);
  props.metadata = asPtr(detailMetadata);
  props.headingText = theme.bodyText;
  props.bodyText = theme.bodyText;
  props.primary.label = acquireLabel();
  props.primary.action = ACTION_DETAIL;
  props.actionHeight = theme.rowHeight;

  fui::publicationPage(screen.frame(), screen.body(), props);
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

// Login can take several seconds (TLS handshakes across the catalog and its
// SSO); reflect the client's phase in the status line so the screen isn't
// frozen on the previous message.
void OpdsBookBrowserActivity::onClientStatus(void* ctx, const freeink::opds::ClientPhase phase) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(ctx);
  self->statusMessage = phase == freeink::opds::ClientPhase::SigningIn ? tr(STR_OPDS_SIGNING_IN) : tr(STR_LOADING);
  self->requestUpdate(true);
}

bool OpdsBookBrowserActivity::persistTokens() {
  OpdsTokens tokens;
  tokens.accessToken = opdsClient.accessToken();
  tokens.refreshToken = opdsClient.refreshToken();
  tokens.refreshUrl = opdsClient.refreshUrl();
  const bool persisted = OPDS_TOKENS.put(tokenKey(), tokens);
  if (!persisted) {
    // Best-effort cache: on failure the reader just re-authenticates next
    // session. Report it so the caller can detect it, rather than swallowing it.
    LOG_ERR("OPDS", "Failed to persist OPDS tokens for %s", server.url.c_str());
  }
  return persisted;
}

// Resolves the search template lazily: OPDS 1.x servers such as calibre-web,
// COPS and Kavita publish it in a separate OpenSearch description document
// instead of inlining it in the feed.
bool OpdsBookBrowserActivity::ensureSearchTemplate() {
  if (!searchTemplate.empty()) return true;
  if (searchDescriptionUrl.empty()) return false;

  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  const std::string descUrl = UrlUtils::buildUrl(feedUrl, searchDescriptionUrl);
  std::string tmpl;
  if (!opdsClient.fetchSearchTemplate(descUrl, tmpl)) return false;
  searchTemplate = std::move(tmpl);
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
  const std::string expanded = expandOpdsSearchTemplate(searchTemplate, opdsPercentEncode(query));
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
