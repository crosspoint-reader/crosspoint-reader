#include "OpdsBookBrowserActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>
#include <Xtc.h>
#include <expat.h>

#include <cctype>
#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "OpdsFormatLabel.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int FORMAT_ITEM_HEIGHT = 30;
constexpr int FORMAT_LIST_TOP_OFFSET = 20;
constexpr int FORMAT_LIST_BOTTOM_PADDING = 20;

int formatItemsPerPage(const Rect& contentRect) {
  const int midY = contentRect.y + contentRect.height / 2;
  const int listTop = midY + FORMAT_LIST_TOP_OFFSET;
  const int listBottom = contentRect.y + contentRect.height - FORMAT_LIST_BOTTOM_PADDING;
  const int rawAvailableHeight = listBottom - listTop;
  const int availableHeight = rawAvailableHeight > FORMAT_ITEM_HEIGHT ? rawAvailableHeight : FORMAT_ITEM_HEIGHT;
  const int itemsPerPage = availableHeight / FORMAT_ITEM_HEIGHT;
  return itemsPerPage > 0 ? itemsPerPage : 1;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  currentPath = "";  // Root path - user provides full URL in settings
  searchTemplate.clear();
  selectorIndex = 0;
  selectedBookIndex = -1;
  formatSelectorIndex = 0;
  formatSelectionLabels.clear();
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();

  HalClock::wifiOff();

  entries.clear();
  navigationHistory.clear();
  formatSelectionLabels.clear();
}

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

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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

  if (state == BrowserState::FORMAT_SELECTION) {
    if (selectedBookIndex < 0 || selectedBookIndex >= static_cast<int>(entries.size())) {
      state = BrowserState::BROWSING;
      requestUpdate();
      return;
    }

    const auto& entry = entries[selectedBookIndex];
    if (entry.acquisitionLinks.empty()) {
      state = BrowserState::BROWSING;
      selectedBookIndex = -1;
      formatSelectionLabels.clear();
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;
      selectedBookIndex = -1;
      formatSelectionLabels.clear();
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      downloadBook(entry, entry.acquisitionLinks[formatSelectorIndex]);
      return;
    }

    buttonNavigator.onNextRelease([this, &entry] {
      formatSelectorIndex = ButtonNavigator::nextIndex(formatSelectorIndex, entry.acquisitionLinks.size());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this, &entry] {
      formatSelectorIndex = ButtonNavigator::previousIndex(formatSelectorIndex, entry.acquisitionLinks.size());
      requestUpdate();
    });
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? chooseBookFormat(entry) : navigateToEntry(entry);
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

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int midY = contentRect.y + contentRect.height / 2;

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 40, tr(STR_DOWNLOADING));
    // Trim long titles to keep them within the content bounds.
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), contentRect.width - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 10, title.c_str());
    if (downloadTotal > 0) {
      const int barWidth = contentRect.width - 100;
      constexpr int barHeight = 20;
      const int barX = contentRect.x + 50;
      const int barY = midY + 20;
      GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::FORMAT_SELECTION) {
    const auto& entry = entries[selectedBookIndex];
    auto title = renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), contentRect.width - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 40, title.c_str(), true, EpdFontFamily::BOLD);
    if (!entry.author.empty()) {
      auto author = renderer.truncatedText(UI_10_FONT_ID, entry.author.c_str(), contentRect.width - 40);
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 10, author.c_str());
    }

    const int listTop = midY + 20;
    const int itemsPerPage = formatItemsPerPage(contentRect);
    const int pageStartIndex = formatSelectorIndex / itemsPerPage * itemsPerPage;
    renderer.fillRect(contentRect.x, listTop + (formatSelectorIndex - pageStartIndex) * FORMAT_ITEM_HEIGHT - 2,
                      contentRect.width - 1, FORMAT_ITEM_HEIGHT);
    for (int i = pageStartIndex;
         i < static_cast<int>(entry.acquisitionLinks.size()) && i < pageStartIndex + itemsPerPage; i++) {
      const char* label = i < static_cast<int>(formatSelectionLabels.size()) ? formatSelectionLabels[i].c_str() : "";
      auto item = renderer.truncatedText(UI_10_FONT_ID, label, contentRect.width - 40);
      renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, listTop + (i - pageStartIndex) * FORMAT_ITEM_HEIGHT,
                        item.c_str(), i != formatSelectorIndex);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Browsing state
  // Show appropriate button hint based on selected entry type
  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_NO_ENTRIES));
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(contentRect.x, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, contentRect.width - 1, 30);

  for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
    const auto& entry = entries[i];

    // Format display text with type indicator
    std::string displayText;
    if (entry.type == OpdsEntryType::NAVIGATION) {
      displayText = "> " + entry.title;  // Folder/navigation indicator
    } else {
      // Book: "Title - Author" or just "Title"
      displayText = entry.title;
      if (!entry.author.empty()) {
        displayText += " - " + entry.author;
      }
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), contentRect.width - 40);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                      i != static_cast<size_t>(selectorIndex));
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

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
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
  if (searchTemplate.empty() && !parser.getOsdUrl().empty()) {
    fetchOsdTemplate(UrlUtils::buildUrl(url, parser.getOsdUrl()));
  }
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
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  currentPath = entry.href;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
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
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

// Opens a screen to allow the user to choose which format they want to download
// if multiple formats are available. If only one format is available, the
// download is started immediately.
void OpdsBookBrowserActivity::chooseBookFormat(const OpdsEntry& book) {
  if (book.acquisitionLinks.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  if (book.acquisitionLinks.size() == 1) {
    downloadBook(book, book.acquisitionLinks[0]);
    return;
  }

  selectedBookIndex = selectorIndex;
  formatSelectorIndex = 0;
  formatSelectionLabels = buildOpdsFormatSelectionLabels(book.acquisitionLinks, SETTINGS.opdsServerUrl);
  state = BrowserState::FORMAT_SELECTION;
  requestUpdate();
}

// Downloads the selected book's acquisition link and saves it to the SD card.
void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book, const OpdsAcquisitionLink& acquisition) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = 0;
  downloadTotal = 0;
  requestUpdate(true);

  std::string downloadUrl = (acquisition.href.rfind("http", 0) == 0)
                                ? acquisition.href
                                : UrlUtils::buildUrl(SETTINGS.opdsServerUrl, acquisition.href);
  std::string filename = "/" +
                         StringUtils::sanitizeFilename(book.title + (book.author.empty() ? "" : " - " + book.author)) +
                         acquisition.fileExtension;

  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      server.username, server.password);

  if (result == HttpDownloader::OK) {
    FsFile downloadedFile;
    if (!Storage.openFileForRead("OPDS", filename, downloadedFile) || downloadedFile.size() == 0) {
      LOG_ERR("OPDS", "Downloaded file is empty or unreadable: %s", filename.c_str());
      if (downloadedFile) {
        downloadedFile.close();
      }
      Storage.remove(filename.c_str());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    downloadedFile.close();

    LOG_DBG("OPDS", "Download complete: %s", filename.c_str());

    // Clear any existing cache for this book just in case it's a redownload of
    // a previously opened book.
    if (acquisition.mimeType == "application/epub+zip") {
      Epub(filename, "/.crosspoint").clearCache();
    } else if (acquisition.formatKey == "xtc" || acquisition.formatKey == "xtch") {
      Xtc(filename, "/.crosspoint").clearCache();
    }
    selectedBookIndex = -1;
    formatSelectionLabels.clear();
    state = BrowserState::BROWSING;
    requestUpdate();
  } else {
    selectedBookIndex = -1;
    formatSelectionLabels.clear();
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::fetchOsdTemplate(const std::string& osdUrl) {
  std::string content;
  if (!HttpDownloader::fetchUrl(osdUrl, content)) {
    LOG_ERR("OPDS", "Failed to fetch OSD: %s", osdUrl.c_str());
    return;
  }

  struct OsdState {
    std::string templateUrl;
    static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
      if (strcmp(name, "Url") != 0 && strstr(name, ":Url") == nullptr) return;
      auto* state = static_cast<OsdState*>(ud);
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "template") == 0 && strstr(atts[i + 1], "{searchTerms}") != nullptr) {
          state->templateUrl = atts[i + 1];
          break;
        }
      }
    }
  } osdState;

  XML_Parser p = XML_ParserCreate(nullptr);
  if (!p) {
    LOG_ERR("OPDS", "OSD parser alloc failed");
    return;
  }
  XML_SetUserData(p, &osdState);
  XML_SetElementHandler(p, OsdState::onStart, nullptr);
  XML_Parse(p, content.c_str(), static_cast<int>(content.size()), XML_TRUE);
  XML_ParserFree(p);

  if (!osdState.templateUrl.empty()) {
    searchTemplate = UrlUtils::buildUrl(osdUrl, osdState.templateUrl);
  }
}

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

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  // Already connected? Verify connection is valid by checking IP
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
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
