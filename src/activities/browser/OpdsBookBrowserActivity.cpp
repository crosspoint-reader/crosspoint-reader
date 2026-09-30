#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsFeedParser.h>
#include <OpdsPublicationDoc.h>
#include <OpdsSearchTemplate.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsTokenStore.h"
#include "components/UITheme.h"
#include "components/icons/opdsIcons.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
// ACTION_PAGE values: which pagination link a tab follows.
constexpr int16_t PAGE_FIRST = 0;
constexpr int16_t PAGE_PREV = 1;
constexpr int16_t PAGE_NEXT = 2;
constexpr int16_t PAGE_LAST = 3;

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
    : CatalogActivity("OpdsBookBrowser", renderer, mappedInput), server(std::move(server)) {}

void OpdsBookBrowserActivity::onEnter() {
  CatalogActivity::onEnter();
  app.on(ACTION_PAGE, &OpdsBookBrowserActivity::onPageEvent, this);
  app.on(ACTION_DETAIL, &OpdsBookBrowserActivity::onDetailEvent, this);

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

  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  releaseEntries();
  navigationHistory.clear();
  if (!detailCoverPath.empty()) {
    if (Storage.exists(detailCoverPath.c_str())) Storage.remove(detailCoverPath.c_str());
    detailCoverPath.clear();
  }
  detailCoverReady = false;
  CatalogActivity::onExit();
}

void OpdsBookBrowserActivity::activateIndex(const int index) {
  app.clearTapFlash();
  const auto& entry = entries[index];
  entry.type == OpdsEntryType::BOOK ? openPublicationDetail(entry) : navigateToEntry(entry);
}

bool OpdsBookBrowserActivity::handleCustomInput() {
  if (state == State::DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      downloadBook(detailBook);
      return true;
    }
    // Back falls through to handleButtons() -> onBackButton(); the acquire
    // button routes through the shared touch pass (ACTION_DETAIL).
    return false;
  }
  if (state == State::BROWSING &&
      (!pageNextHref.empty() || !pagePrevHref.empty() || !pageFirstHref.empty() || !pageLastHref.empty())) {
    // On a paginated feed the side buttons belong to feed pages exclusively.
    // NavNext/NavPrevious resolve to the side buttons too, so every side
    // press/hold pass is swallowed here or the shared list nav would also
    // step the selection under the same physical button.
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      if (!pageNextHref.empty()) followPageLink(pageNextHref);
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      if (!pagePrevHref.empty()) followPageLink(pagePrevHref);
      return true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
        mappedInput.isPressed(MappedInputManager::Button::PageBack)) {
      return true;
    }
  }
  return CatalogActivity::handleCustomInput();
}

void OpdsBookBrowserActivity::onPageEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != State::BROWSING) return;
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
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
}

void OpdsBookBrowserActivity::buildScreen(UiScreen& screen) {
  // An active search replaces the server name with the quoted query, like the
  // library view, so the reader can see what produced the current list. With
  // no search, a navigated feed's own title beats the server name.
  const char* title = !headerSearchTitle.empty() ? headerSearchTitle.c_str()
                      : !feedTitle.empty()       ? feedTitle.c_str()
                      : server.name.empty()      ? tr(STR_OPDS_BROWSER)
                                                 : server.name.c_str();
  screenHeader(screen, title);
  if (state == State::DETAIL) {
    buildDetailScreen(screen);
    return;
  }
  if (buildStatusScreen(screen, /*boldError=*/false, /*showDownloadTotal=*/true)) return;
  buildBrowsingScreen(screen);
}

// Bottom pagination bar: arrow-icon tabs following the feed's first/prev/next/
// last links. Prev and Next always show when the feed is paginated (the
// unavailable direction is disabled); First/Last appear only when advertised.
void OpdsBookBrowserActivity::buildPaginationBar(UiScreen& screen) {
  // Backward arrows on the left, forward arrows on the right, "Page N of M"
  // between them when the feed reports its pagination.
  fui::TabItem left[2], right[2];
  int lc = 0, rc = 0;
  const auto addTab = [](fui::TabItem* tabs, int& count, const freeink::Icon& icon, const int16_t value,
                         const std::string& href) {
    tabs[count].icon = fui::bitmapFromIcon(icon);
    tabs[count].value = value;
    tabs[count].enabled = !href.empty();
    ++count;
  };
  if (!pageFirstHref.empty()) addTab(left, lc, icon_page_first_32, PAGE_FIRST, pageFirstHref);
  addTab(left, lc, icon_page_prev_32, PAGE_PREV, pagePrevHref);
  addTab(right, rc, icon_page_next_32, PAGE_NEXT, pageNextHref);
  if (!pageLastHref.empty()) addTab(right, rc, icon_page_last_32, PAGE_LAST, pageLastHref);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect band = screen.takeBottom(static_cast<int16_t>(metrics.tabBarHeight));
  // Full-width band with a top rule, matching the tab chrome elsewhere.
  const fui::Rect frameRect = screen.frame().screen();
  const fui::Rect barRect{frameRect.x, band.y, frameRect.width, band.height};
  screen.target().fill(fui::Rect{barRect.x, barRect.y, barRect.width, 1}, fui::Paint::solid(fui::Color::Black));

  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const int16_t zoneW = static_cast<int16_t>((barRect.width - 2 * side) / 3);
  const int16_t slotY = static_cast<int16_t>(barRect.y + 1);
  const int16_t slotH = static_cast<int16_t>(barRect.height - 1);
  fui::TabBarProps props;
  props.action = ACTION_PAGE;
  props.inputMask = fui::InputTouch;
  props.iconSize = 32;
  props.tabs = left;
  props.count = static_cast<uint16_t>(lc);
  fui::tabBar(screen.frame(), fui::Rect{static_cast<int16_t>(barRect.x + side), slotY, zoneW, slotH}, props);
  props.tabs = right;
  props.count = static_cast<uint16_t>(rc);
  fui::tabBar(screen.frame(), fui::Rect{static_cast<int16_t>(barRect.right() - side - zoneW), slotY, zoneW, slotH},
              props);

  if (pageCurrent > 0 && pageTotal > 0) {
    char pageText[32];
    snprintf(pageText, sizeof(pageText), "%d/%d", pageCurrent, pageTotal);
    fui::TextStyle centered = screen.theme().smallText;
    centered.align = fui::TextAlign::Center;
    const int16_t lh = screen.target().lineHeight(centered.font);
    screen.target().text(fui::Rect{static_cast<int16_t>(barRect.x + side + zoneW),
                                   static_cast<int16_t>(slotY + (slotH - lh) / 2), zoneW, lh},
                         pageText, centered);
  }
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  // Reserve the pagination band before the list claims the remaining height.
  if (!pageNextHref.empty() || !pagePrevHref.empty() || !pageFirstHref.empty() || !pageLastHref.empty()) {
    buildPaginationBar(screen);
  }

  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // rowItems is built whenever entries changes (see rebuildRowItems(), called
  // from fetchFeed()/releaseEntries()) and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

void OpdsBookBrowserActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      const char* confirmLabel = tr(STR_OPEN);
      if (!entries.empty() && nav.selected >= 0 && nav.selected < static_cast<int>(entries.size()) &&
          entries[nav.selected].type == OpdsEntryType::BOOK) {
        confirmLabel = entries[nav.selected].purchase ? tr(STR_OPDS_BUY) : tr(STR_DOWNLOAD);
      }
      const char* searchLabel = (hasSearch() && nav.selected == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case State::DETAIL:
      labels = mappedInput.mapLabels(tr(STR_BACK), acquireLabel(), "", "");
      break;
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Decode the book cover into the rect the layout reserved. Done here — the
  // last render hook before displayBuffer() — so the JPEG decoder's stack
  // cost doesn't stack on the deep FreeInkUI compose path.
  if (state == State::DETAIL && detailCoverReady && detailCoverRect.width > 0 && detailCoverRect.height > 0) {
    if (ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(detailCoverPath)) {
      RenderConfig cfg{detailCoverRect.x, detailCoverRect.y, detailCoverRect.width, detailCoverRect.height};
      decoder->decodeToFramebuffer(detailCoverPath, renderer, cfg);
    }
  }
}

void OpdsBookBrowserActivity::startBrowse() {
  releaseEntries();  // harmless when already empty
  nav.reset();
  beginLoading();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::downloadFinished(bool) {
  // Reload the released catalog (entries were freed for the TLS session).
  startBrowse();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    fail(StrId::STR_NO_SERVER_URL);
    return;
  }

  const std::string url = UrlUtils::buildUrl(server.url, path);
  OpdsFeedParser parser;
  const auto status = opdsClient.fetchFeed(url, parser);
  // The token state may have changed (renewed, or cleared on failure); persist
  // only when an auth handshake actually ran.
  if (opdsClient.tokensDirty()) persistTokens();
  if (status != freeink::opds::OpdsClient::FetchStatus::Ok) {
    switch (status) {
      case freeink::opds::OpdsClient::FetchStatus::CredentialsMissing:
        fail(StrId::STR_SET_CREDENTIALS_FIRST);
        break;
      case freeink::opds::OpdsClient::FetchStatus::AuthFailed:
        fail(StrId::STR_OPDS_AUTH_FAILED);
        break;
      case freeink::opds::OpdsClient::FetchStatus::ParseFailed:
        fail(StrId::STR_PARSE_FEED_FAILED);
        break;
      default:
        fail(StrId::STR_FETCH_FEED_FAILED);
        break;
    }
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
  pageCurrent = parser.currentPage();
  pageTotal = parser.pageCount();
  const bool feedTruncated = parser.truncated();
  // Reset the selection before the swap: the render task reads the selected
  // entry under only an empty() guard, and the new feed can be shorter than
  // the old selection.
  nav.reset();
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

  state = entries.empty() ? State::ERROR : State::BROWSING;
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

  startBrowse();
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
    return;
  }
  currentPath = navigationHistory.back();
  navigationHistory.pop_back();
  if (!searchQueryHistory.empty()) {
    setSearchQuery(searchQueryHistory.back());
    searchQueryHistory.pop_back();
  }
  startBrowse();
}

void OpdsBookBrowserActivity::onBackButton() {
  if (state == State::DETAIL) {
    // Entries were released for heap; rebuild the catalog we came from.
    startBrowse();
    return;
  }
  navigateBack();
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  beginDownload(book.title);

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
      fail(StrId::STR_OPDS_NOT_A_BOOK);
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

  std::vector<HttpDownloader::Header> headers;
  const freeink::opds::HttpAuth dlAuth = opdsClient.downloadAuth();
  if (!dlAuth.bearer.empty()) headers.push_back({"Authorization", "Bearer " + dlAuth.bearer});
  // downloadFile() (CatalogActivity) releases font caches, checks the TLS heap
  // floor, and pumps cancel input during the transfer.
  const auto result = downloadFile(downloadUrl, filename, dlAuth.username, dlAuth.password, headers);

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
      fail(StrId::STR_OPDS_NOT_A_BOOK);
      return;
    }
    clearBookCache(filename);
    library::markLibraryIndexDirty();
  }
  finishDownload(result);
}

void OpdsBookBrowserActivity::onDetailEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != State::DETAIL) return;
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

  beginLoading();

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
  // Feed-inline fallbacks: without a publication self document (OPDS 1.x, or
  // OPDS 2.0 servers like Mayberry that inline everything) the feed entry is
  // the only source for the description and cover. Also fills gaps when a
  // document exists but omits one of them.
  if (currentPublication.description.empty()) currentPublication.description = entry.description;
  if (currentPublication.coverHref.empty()) currentPublication.coverHref = entry.coverHref;

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
  state = State::DETAIL;
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
  std::vector<HttpDownloader::Header> headers;
  const freeink::opds::HttpAuth auth = opdsClient.downloadAuth();
  if (!auth.bearer.empty()) headers.push_back({"Authorization", "Bearer " + auth.bearer});
  const auto result = HttpDownloader::downloadToFile(url, tmp, nullptr, nullptr, auth.username, auth.password, headers);
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
  if (!self->detailCoverReady) {
    // No cover art: the same placeholder the home screen draws (cheap, so it
    // can run right here, unlike the JPEG decode).
    GUI.drawCoverPlaceholder(self->renderer, Rect{rect.x, rect.y, rect.width, rect.height});
    self->detailCoverRect = fui::Rect{};
    return true;
  }
  // Record the rect only; the decode runs in drawFooter() to keep the JPEG
  // decoder off this deep component call chain.
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
  // Content band comes from screenHeader() (header above, button hints
  // below), same as every other state.
  const auto& theme = screen.theme();
  const auto asPtr = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };

  fui::PublicationPageProps props;
  props.book.title = asPtr(currentPublication.title);
  props.book.author = asPtr(currentPublication.author);
  // Downloaded cover: the painter reserves its rect and drawFooter() decodes
  // into it. No cover: the painter draws the home screen's placeholder.
  props.book.coverPainter = &OpdsBookBrowserActivity::detailCoverPainter;
  props.book.coverPainterUserData = this;
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
  // Same styling and theme rounding as every themed button in the app
  // (controlRadius: RoundedRaff pill, Classic square), plus an outline so
  // the resting state reads as a button against the page.
  props.primary.styles = theme.button;
  props.primary.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.primary.styles.normal.borderWidth = 1;
  props.primary.text = theme.bodyText;
  props.primary.radius = theme.controlRadius;
  props.primary.minTouchSize = theme.minTouchSize;
  props.actionHeight = theme.rowHeight;

  fui::publicationPage(screen.frame(), screen.body(), props);
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
    state = State::BROWSING;
    requestUpdate();
    return;
  }

  beginLoading();

  if (!ensureSearchTemplate()) {
    fail(StrId::STR_FETCH_FEED_FAILED);
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

  startBrowse();
}
