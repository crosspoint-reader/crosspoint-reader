#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr size_t OPDS_PROGRESS_UPDATE_BYTES = 1024 * 1024;

const char* acquisitionBadge(const OpdsAcquisitionType type) {
  switch (type) {
    case OpdsAcquisitionType::EPUB:
      return "[e] ";
    case OpdsAcquisitionType::XTC:
    case OpdsAcquisitionType::XTCH:
      return "[x] ";
    case OpdsAcquisitionType::UNKNOWN:
      return "";
  }
  return "";
}

const char* acquisitionExtension(const OpdsAcquisitionType type) {
  switch (type) {
    case OpdsAcquisitionType::XTC:
      return ".xtc";
    case OpdsAcquisitionType::XTCH:
      return ".xtch";
    case OpdsAcquisitionType::EPUB:
    case OpdsAcquisitionType::UNKNOWN:
      return ".epub";
  }
  return ".epub";
}

std::string bookDisplayText(const OpdsEntry& entry) {
  std::string text = acquisitionBadge(entry.acquisitionType);
  if (!entry.author.empty()) {
    text += entry.author + " - ";
  }
  text += entry.title;
  return text;
}

std::string bookDownloadBaseName(const OpdsEntry& entry) {
  return (entry.author.empty() ? "" : entry.author + " - ") + entry.title;
}

std::string opdsDownloadErrorCode(const HttpDownloader::DownloadError error) {
  switch (error.result) {
    case HttpDownloader::HTTP_ERROR:
      if (error.httpStatus > 0 && error.httpStatus != 200) {
        return "error 2001-" + std::to_string(error.httpStatus);
      }
      return "error 2001";
    case HttpDownloader::FILE_ERROR:
      return "error 2002";
    case HttpDownloader::ABORTED:
      return "error 2003";
    case HttpDownloader::OK:
      return "";
  }
  return "error 2099";
}

std::string opdsHttpStatusDetail(const HttpDownloader::DownloadError error) {
  if (error.httpStatus > 0 && error.httpStatus != 200) {
    return "http " + std::to_string(error.httpStatus);
  }
  return "";
}

std::string downloadSpeedText(const size_t downloadedBytes, const size_t totalBytes, const uint32_t kibPerSec) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%zu/%zu KiB \xE2\x80\xA2 %u KiB/s", downloadedBytes / 1024, totalBytes / 1024,
           static_cast<unsigned>(kibPerSec));
  return buf;
}

std::string downloadIntervalText(const size_t intervalBytes, const uint32_t intervalMs, const int rssi) {
  char buf[80];
  snprintf(buf, sizeof(buf), "%zu KiB / %u ms  RSSI %d dBm", intervalBytes / 1024,
           static_cast<unsigned>(intervalMs), rssi);
  return buf;
}

std::string downloadNetworkTimeText(const bool usesTls, const uint32_t readMs) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s read %u ms", usesTls ? "net+TLS" : "net+HTTP", static_cast<unsigned>(readMs));
  return buf;
}

std::string downloadSdTimeText(const uint32_t writeMs) {
  char buf[32];
  snprintf(buf, sizeof(buf), "SD write %u ms", static_cast<unsigned>(writeMs));
  return buf;
}
}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  errorDetail.clear();
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

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
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
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
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
    const int messageY = errorDetail.empty() ? pageHeight / 2 + 10 : pageHeight / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, messageY, errorMessage.c_str());
    if (!errorDetail.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, errorDetail.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    const int barY = pageHeight / 2 + 20;
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, barY, pageWidth - 100, 20}, downloadProgress, downloadTotal);
    }
    if (downloadShowDebugInfo && downloadStatsAvailable) {
      const int statsY = barY + 20 + 64;
      auto speedText = renderer.truncatedText(
          SMALL_FONT_ID, downloadSpeedText(downloadProgress, downloadTotal, downloadKibPerSec).c_str(), pageWidth - 40);
      renderer.drawCenteredText(SMALL_FONT_ID, statsY, speedText.c_str());
      auto intervalText = renderer.truncatedText(
          SMALL_FONT_ID, downloadIntervalText(downloadIntervalBytes, downloadIntervalMs, downloadRssi).c_str(),
          pageWidth - 40);
      renderer.drawCenteredText(SMALL_FONT_ID, statsY + 24, intervalText.c_str());
      auto networkText = renderer.truncatedText(
          SMALL_FONT_ID, downloadNetworkTimeText(downloadUsesTls, downloadReadMs).c_str(), pageWidth - 40);
      renderer.drawCenteredText(SMALL_FONT_ID, statsY + 48, networkText.c_str());
      auto sdText =
          renderer.truncatedText(SMALL_FONT_ID, downloadSdTimeText(downloadWriteMs).c_str(), pageWidth - 40);
      renderer.drawCenteredText(SMALL_FONT_ID, statsY + 72, sdText.c_str());
    }
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& entry = entries[i];
      std::string displayText =
          (entry.type == OpdsEntryType::NAVIGATION) ? "> " + entry.title : bookDisplayText(entry);
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    errorDetail.clear();
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    const auto result = HttpDownloader::fetchUrl(url, stream, server.username, server.password);
    if (result.result != HttpDownloader::OK) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      errorDetail = opdsHttpStatusDetail(result);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    errorDetail.clear();
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
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) {
    errorMessage = tr(STR_NO_ENTRIES);
    errorDetail.clear();
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
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

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  errorDetail.clear();
  downloadProgress = downloadTotal = 0;
  downloadIntervalBytes = 0;
  downloadIntervalMs = 0;
  downloadReadMs = 0;
  downloadWriteMs = 0;
  downloadKibPerSec = 0;
  downloadRssi = 0;
  downloadUsesTls = false;
  downloadStatsAvailable = false;
  downloadShowDebugInfo = SETTINGS.opdsShowDebugInfo != 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  std::string filename =
      "/" + StringUtils::sanitizeFilename(bookDownloadBaseName(book)) + acquisitionExtension(book.acquisitionType);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  size_t lastRenderBytes = 0;
  size_t lastStatsBytes = 0;
  uint32_t lastStatsMs = millis();
  uint32_t statsReadMs = 0;
  uint32_t statsWriteMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRenderBytes, &lastStatsBytes, &lastStatsMs, &statsReadMs, &statsWriteMs](
          const HttpDownloader::ProgressInfo& info) {
        const size_t downloaded = info.downloaded;
        const size_t total = info.total;
        downloadProgress = downloaded;
        downloadTotal = total;
        statsReadMs += info.readMs;
        statsWriteMs += info.writeMs;
        const bool complete = total > 0 && downloaded >= total;

        if (downloadShowDebugInfo &&
            (downloaded - lastStatsBytes >= OPDS_PROGRESS_UPDATE_BYTES || complete)) {
          const size_t intervalBytes = downloaded - lastStatsBytes;
          if (intervalBytes > 0) {
            const uint32_t now = millis();
            const uint32_t intervalMs = std::max<uint32_t>(1, now - lastStatsMs);
            downloadIntervalBytes = intervalBytes;
            downloadIntervalMs = intervalMs;
            downloadReadMs = statsReadMs;
            downloadWriteMs = statsWriteMs;
            downloadKibPerSec = static_cast<uint32_t>(
                (static_cast<uint64_t>(intervalBytes) * 1000ULL) / (static_cast<uint64_t>(intervalMs) * 1024ULL));
            downloadRssi = WiFi.RSSI();
            downloadUsesTls = info.usesTls;
            downloadStatsAvailable = true;
            lastStatsBytes = downloaded;
            lastStatsMs = now;
            statsReadMs = 0;
            statsWriteMs = 0;
          }
        }

        if (downloaded - lastRenderBytes >= OPDS_PROGRESS_UPDATE_BYTES || complete) {
          lastRenderBytes = downloaded;
          requestUpdate(true);
        }
      },
      nullptr, server.username, server.password);

  if (result.result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    errorDetail = opdsDownloadErrorCode(result);
  }
  requestUpdate();
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

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
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
    errorDetail.clear();
    requestUpdate();
  }
}
