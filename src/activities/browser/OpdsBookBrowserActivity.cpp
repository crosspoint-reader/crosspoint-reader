#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  fromCache = false;
  pendingWifiAction = PostWifiAction::NONE;

  // Show cached feed immediately if available — no WiFi required
  if (server.cacheEnabled && feedCache.load(server.url)) {
    buildCachedEntries();
    state = BrowserState::CACHED;
    requestUpdate();
    return;
  }

  // No cache yet: connect to WiFi and load the feed for the first time
  state = BrowserState::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
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

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  // --- CACHED state: show offline list, explicit actions for WiFi ops ---
  if (state == BrowserState::CACHED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectorIndex == 0) {
        requireWifi(PostWifiAction::REFRESH_FEED);
      } else if (selectorIndex == 1) {
        requireWifi(PostWifiAction::SYNC_BOOKS);
      } else if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.type == OpdsEntryType::BOOK) {
          const std::string path = getDownloadedPath(entry);
          if (!path.empty()) {
            openBook(path, BrowserState::CACHED);
          } else {
            requireWifi(PostWifiAction::DOWNLOAD_ENTRY, &entry);
          }
        } else {
          requireWifi(PostWifiAction::NAVIGATE_ENTRY, &entry);
        }
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    if (!entries.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        // Set FETCH_CURRENT so onWifiReady knows to re-fetch after reconnecting
        pendingWifiAction = PostWifiAction::FETCH_CURRENT;
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

  if (state == BrowserState::DOWNLOADING || state == BrowserState::SYNCING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.type == OpdsEntryType::BOOK) {
          const std::string path = getDownloadedPath(entry);
          if (!path.empty()) {
            openBook(path, BrowserState::BROWSING);
          } else {
            downloadBook(entry);
          }
        } else {
          navigateToEntry(entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    if (!entries.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::SYNCING || state == BrowserState::DOWNLOADING) {
    const char* stateLabel = (state == BrowserState::SYNCING) ? tr(STR_SYNCING_ARTICLES) : tr(STR_DOWNLOADING);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, stateLabel);
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // CACHED or BROWSING: render entry list
  const bool inCached = (state == BrowserState::CACHED);

  // Determine confirm label
  const char* confirmLabel = tr(STR_SELECT);
  if (!entries.empty()) {
    if (inCached && selectorIndex < actionItemCount) {
      confirmLabel = tr(STR_SELECT);
    } else {
      const auto& sel = entries[selectorIndex];
      if (sel.type == OpdsEntryType::BOOK) {
        confirmLabel = isBookDownloaded(sel) ? tr(STR_OPEN) : tr(STR_DOWNLOAD_AND_OPEN);
      } else {
        confirmLabel = tr(STR_OPEN);
      }
    }
  }

  const char* backLabel = inCached ? tr(STR_HOME) : tr(STR_BACK);

  const char* leftLabel = tr(STR_DIR_UP);
  if (!inCached && !searchTemplate.empty() && selectorIndex == 0) {
    leftLabel = tr(STR_SEARCH);
  }

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, leftLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& entry = entries[i];
      std::string displayText;

      if (inCached && static_cast<int>(i) < actionItemCount) {
        // Virtual action items — render like navigation entries
        displayText = "> " + entry.title;
      } else if (entry.type == OpdsEntryType::NAVIGATION) {
        displayText = "> " + entry.title;
      } else {
        displayText = (isBookDownloaded(entry) ? "* " : "  ") + entry.title;
        if (!entry.author.empty()) displayText += " - " + entry.author;
      }

      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// WiFi / action dispatch
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::requireWifi(PostWifiAction action, const OpdsEntry* entry) {
  pendingWifiAction = action;
  if (entry) pendingEntry = *entry;

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    onWifiReady();
  } else {
    launchWifiSelection();
  }
}

void OpdsBookBrowserActivity::onWifiReady() {
  const PostWifiAction action = pendingWifiAction;
  pendingWifiAction = PostWifiAction::NONE;

  switch (action) {
    case PostWifiAction::FETCH_CURRENT: {
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate();
      fetchFeed(currentPath);
      break;
    }
    case PostWifiAction::REFRESH_FEED: {
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate();
      fetchFeed("", true);  // root fetch, return to CACHED after
      break;
    }
    case PostWifiAction::SYNC_BOOKS: {
      state = BrowserState::SYNCING;
      statusMessage = "";
      requestUpdate(true);
      syncBooks();
      buildCachedEntries();
      state = BrowserState::CACHED;
      requestUpdate();
      break;
    }
    case PostWifiAction::DOWNLOAD_ENTRY: {
      downloadBook(pendingEntry);
      break;
    }
    case PostWifiAction::NAVIGATE_ENTRY: {
      fromCache = true;
      navigateToEntry(pendingEntry);
      break;
    }
    case PostWifiAction::NONE:
      break;
  }
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath, false);
    return;
  }
  // Use FETCH_CURRENT so onWifiReady knows to fetch after connecting
  pendingWifiAction = PostWifiAction::FETCH_CURRENT;
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
    onWifiReady();
  } else {
    pendingWifiAction = PostWifiAction::NONE;
    // If we have a cached feed, return to it rather than showing an error
    if (feedCache.isLoaded()) {
      buildCachedEntries();
      state = BrowserState::CACHED;
      requestUpdate();
      return;
    }
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Feed fetching
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::fetchFeed(const std::string& path, bool returnToCached) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  const std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = std::move(parser).getEntries();

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }

  selectorIndex = 0;

  if (entries.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_ENTRIES);
    requestUpdate();
    return;
  }

  // Save to cache for root-level fetches (only when caching is enabled for this server)
  const bool isRootFetch = path.empty() || path == server.url;
  if (server.cacheEnabled && isRootFetch) {
    feedCache.save(server.url, entries);
  }

  if (returnToCached && server.cacheEnabled) {
    buildCachedEntries();
    state = BrowserState::CACHED;
  } else {
    state = BrowserState::BROWSING;
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    if (fromCache) {
      // Return to cached list instead of going home
      buildCachedEntries();
      state = BrowserState::CACHED;
      fromCache = false;
      requestUpdate();
    } else {
      onGoHome();
    }
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

// ---------------------------------------------------------------------------
// CACHED state helpers
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::buildCachedEntries() {
  entries.clear();
  actionItemCount = 0;

  // Virtual action items (always present)
  entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_UPDATE_LIST), "", "", ""});
  entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_SYNC_BOOKS), "", "", ""});
  actionItemCount = 2;

  std::transform(feedCache.getEntries().begin(), feedCache.getEntries().end(), std::back_inserter(entries),
                 [](const OpdsCachedEntry& ce) { return ce.entry; });

  // Keep selectorIndex in range
  if (selectorIndex >= static_cast<int>(entries.size())) selectorIndex = 0;
}

// ---------------------------------------------------------------------------
// Download / open
// ---------------------------------------------------------------------------

bool OpdsBookBrowserActivity::doDownload(const std::string& url, const std::string& filename) {
  const auto result = HttpDownloader::downloadToFile(
      url, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      nullptr, server.username, server.password);
  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    return true;
  }
  return false;
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  if (!server.downloadPath.empty()) Storage.mkdir(server.downloadPath.c_str());
  const std::string filename = newPathForBook(book);
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  const std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  if (doDownload(downloadUrl, filename)) {
    if (server.cacheEnabled) feedCache.updateLocalPath(book.id, filename);
    const BrowserState returnState = fromCache ? BrowserState::CACHED : BrowserState::BROWSING;
    if (returnState == BrowserState::CACHED) {
      buildCachedEntries();
    } else {
      state = BrowserState::BROWSING;
      requestUpdate();
    }
    openBook(filename, returnState);
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::syncBooks() {
  if (server.url.empty()) return;

  LOG_INF("OPDS", "Starting sync for server: %s (limit %d)", server.name.c_str(), server.syncLimit);

  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(server.url, stream, server.username, server.password)) {
      LOG_ERR("OPDS", "Sync: failed to fetch feed");
      return;
    }
  }
  if (!parser) {
    LOG_ERR("OPDS", "Sync: failed to parse feed");
    return;
  }

  // Save fresh feed to cache before downloading (only if caching enabled)
  const auto& syncEntries = parser.getEntries();
  if (server.cacheEnabled) feedCache.save(server.url, syncEntries);

  if (!server.downloadPath.empty()) Storage.mkdir(server.downloadPath.c_str());

  int synced = 0;
  for (const auto& entry : syncEntries) {
    if (synced >= server.syncLimit) break;
    if (entry.type != OpdsEntryType::BOOK) continue;
    if (!getDownloadedPath(entry).empty()) continue;

    const std::string filename = newPathForBook(entry);
    statusMessage = entry.title;
    downloadProgress = downloadTotal = 0;
    requestUpdate(true);

    const std::string feedUrl = UrlUtils::buildUrl(server.url, "");
    const std::string downloadUrl = UrlUtils::buildUrl(feedUrl, entry.href);
    LOG_INF("OPDS", "Sync: downloading %s -> %s", downloadUrl.c_str(), filename.c_str());
    if (doDownload(downloadUrl, filename)) {
      if (server.cacheEnabled) feedCache.updateLocalPath(entry.id, filename);
      synced++;
    }
  }
  LOG_INF("OPDS", "Sync complete: %d new book(s) downloaded", synced);
}

void OpdsBookBrowserActivity::openBook(const std::string& path, BrowserState returnState) {
  sdFontSystem.ensureLoaded(renderer);
  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (epub && epub->load(true, SETTINGS.embeddedStyle == 0)) {
    startActivityForResult(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)),
                           [this, returnState](const ActivityResult&) {
                             state = returnState;
                             if (returnState == BrowserState::CACHED) buildCachedEntries();
                             requestUpdate();
                           });
  } else {
    LOG_ERR("OPDS", "Failed to open epub: %s", path.c_str());
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Book path / download state helpers
// ---------------------------------------------------------------------------

std::string OpdsBookBrowserActivity::newPathForBook(const OpdsEntry& entry) const {
  const std::string stem = entry.author.empty() ? entry.title : entry.author + " - " + entry.title;
  return server.downloadPath + "/" + StringUtils::sanitizeFilename(stem) + ".epub";
}

std::string OpdsBookBrowserActivity::getDownloadedPath(const OpdsEntry& entry) const {
  if (entry.type != OpdsEntryType::BOOK) return {};

  // Check cache-recorded path first (matched by stable entry ID)
  if (server.cacheEnabled && !entry.id.empty()) {
    const std::string cached = feedCache.getLocalPath(entry.id);
    if (!cached.empty() && Storage.exists(cached.c_str())) return cached;
  }

  // Computed path from current download folder setting
  const std::string computed = newPathForBook(entry);
  if (Storage.exists(computed.c_str())) return computed;

  // Legacy root fallback for books downloaded before downloadPath was configurable
  // (only needed when the configured folder is not already root)
  if (!server.downloadPath.empty()) {
    const std::string stem = entry.author.empty() ? entry.title : entry.author + " - " + entry.title;
    const std::string legacy = "/" + StringUtils::sanitizeFilename(stem) + ".epub";
    if (Storage.exists(legacy.c_str())) return legacy;
  }

  return {};
}

bool OpdsBookBrowserActivity::isBookDownloaded(const OpdsEntry& entry) const {
  return !getDownloadedPath(entry).empty();
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);
  currentPath = url;

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(url);
}
