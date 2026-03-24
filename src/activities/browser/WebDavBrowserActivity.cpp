#include "WebDavBrowserActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
}

void WebDavBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  currentUrl = UrlUtils::ensureProtocol(SETTINGS.webdavServerUrl);
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void WebDavBrowserActivity::onExit() {
  Activity::onExit();
  WiFi.mode(WIFI_OFF);
  entries.clear();
  navigationHistory.clear();
}

void WebDavBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION) return;

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_WEBDAV_LOADING);
        requestUpdate();
        fetchListing();
      } else {
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

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.isCollection) {
          navigateToEntry(entry);
        } else {
          downloadFile(entry);
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

void WebDavBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_WEBDAV_BROWSER), true, EpdFontFamily::BOLD);

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
    const auto maxWidth = pageWidth - 40;
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), maxWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
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
  const char* confirmLabel = tr(STR_DOWNLOAD);
  if (!entries.empty() && entries[selectorIndex].isCollection) {
    confirmLabel = tr(STR_OPEN);
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
    if (entry.isCollection) {
      displayText = "> " + entry.name;
    } else {
      displayText = entry.name;
      if (entry.contentLength > 0) {
        displayText += " (" + formatSize(entry.contentLength) + ")";
      }
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                      i != static_cast<size_t>(selectorIndex));
  }

  renderer.displayBuffer();
}

void WebDavBrowserActivity::fetchListing() {
  if (currentUrl.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  LOG_DBG("DAV", "Listing: %s", currentUrl.c_str());

  if (!WebDavClient::listFiles(currentUrl.c_str(), SETTINGS.webdavUsername, SETTINGS.webdavPassword, entries)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  LOG_DBG("DAV", "Found %d entries", entries.size());
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

void WebDavBrowserActivity::navigateToEntry(const WebDavEntry& entry) {
  navigationHistory.push_back(currentUrl);

  std::string baseUrl = UrlUtils::extractHost(currentUrl);
  currentUrl = baseUrl + entry.href;

  state = BrowserState::LOADING;
  statusMessage = tr(STR_WEBDAV_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate(true);

  fetchListing();
}

void WebDavBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentUrl = navigationHistory.back();
    navigationHistory.pop_back();

    state = BrowserState::LOADING;
    statusMessage = tr(STR_WEBDAV_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();

    fetchListing();
  }
}

void WebDavBrowserActivity::downloadFile(const WebDavEntry& entry) {
  state = BrowserState::DOWNLOADING;
  statusMessage = entry.name;
  downloadProgress = 0;
  downloadTotal = 0;
  requestUpdate(true);

  std::string baseUrl = UrlUtils::extractHost(currentUrl);
  std::string downloadUrl = baseUrl + entry.href;

  std::string filename = "/" + StringUtils::sanitizeFilename(entry.name);

  LOG_DBG("DAV", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename, SETTINGS.webdavUsername, SETTINGS.webdavPassword,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      });

  if (result == HttpDownloader::OK) {
    LOG_DBG("DAV", "Download complete: %s", filename.c_str());

    Epub epub(filename, "/.crosspoint");
    epub.clearCache();

    state = BrowserState::BROWSING;
    requestUpdate();
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

void WebDavBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_WEBDAV_LOADING);
    requestUpdate();
    fetchListing();
    return;
  }

  launchWifiSelection();
}

void WebDavBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WebDavBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    LOG_DBG("DAV", "WiFi connected, fetching listing");
    state = BrowserState::LOADING;
    statusMessage = tr(STR_WEBDAV_LOADING);
    requestUpdate(true);
    fetchListing();
  } else {
    LOG_DBG("DAV", "WiFi selection cancelled/failed");
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

std::string WebDavBrowserActivity::formatSize(size_t bytes) {
  char buf[16];
  if (bytes >= 1048576) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}
