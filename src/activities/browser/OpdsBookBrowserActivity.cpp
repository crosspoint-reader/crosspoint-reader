#include "OpdsBookBrowserActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  currentPath = "";
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  ActivityWithSubactivity::onExit();

  WiFi.mode(WIFI_OFF);

  entries.clear();
  navigationHistory.clear();
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION) {
    ActivityWithSubactivity::loop();
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        LOG_DBG("OPDS", "Retry: WiFi connected, retrying fetch");
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        LOG_DBG("OPDS", "Retry: WiFi not connected, launching selection");
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.type == OpdsEntryType::BOOK) {
          downloadBook(entry);
        } else {
          navigateToEntry(entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
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

void OpdsBookBrowserActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::LOADING) {
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

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, statusMessage.c_str());
    if (downloadTotal > 0) {
      const int barWidth = pageWidth - 100;
      constexpr int barHeight = 20;
      constexpr int barX = 50;
      const int barY = pageHeight / 2 + 20;
      GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // Browsing state
  const char* confirmLabel = tr(STR_OPEN);
  if (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) {
    confirmLabel = tr(STR_DOWNLOAD);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

  for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
    const auto& entry = entries[i];

    std::string displayText;
    if (entry.type == OpdsEntryType::NAVIGATION) {
      displayText = "> " + entry.title;
    } else {
      displayText = entry.title;
      if (!entry.author.empty()) {
        displayText += " - " + entry.author;
      }
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
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

  std::string url = UrlUtils::buildUrl(server.url, path);
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

  entries = std::move(parser).getEntries();
  LOG_DBG("OPDS", "Found %d entries", entries.size());
  selectorIndex = 0;

  if (entries.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_ENTRIES);
    requestUpdate();
    return;
  }

  state = BrowserState::BROWSING;
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  currentPath = entry.href;

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate();

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
  downloadProgress = 0;
  downloadTotal = 0;
  requestUpdate();

  std::string downloadUrl = UrlUtils::buildUrl(server.url, book.href);

  std::string baseName = book.title;
  if (!book.author.empty()) {
    baseName += " - " + book.author;
  }
  std::string filename = "/" + StringUtils::sanitizeFilename(baseName) + ".epub";

  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate();
      },
      server.username, server.password);

  if (result == HttpDownloader::OK) {
    LOG_DBG("OPDS", "Download complete: %s", filename.c_str());

    Epub epub(filename, "/.crosspoint");
    epub.clearCache();
    LOG_DBG("OPDS", "Cleared cache for: %s", filename.c_str());

    state = BrowserState::BROWSING;
    requestUpdate();
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
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

  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  exitActivity();

  if (connected) {
    LOG_DBG("OPDS", "WiFi connected via selection, fetching feed");
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
  } else {
    LOG_DBG("OPDS", "WiFi selection cancelled/failed");
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
