#include "CatalogActivity.h"

#include <Arduino.h>
#include <FreeInkUIIcon.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CatalogScreens.h"
#include "components/icons/search32.h"

void CatalogActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(ACTION_SEARCH, &CatalogActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &CatalogActivity::onCancelEvent, this);
}

void CatalogActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

bool CatalogActivity::wifiConnected() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void CatalogActivity::fail(const StrId message) {
  state = State::ERROR;
  errorMessage = I18N.get(message);
  requestUpdate();
}

void CatalogActivity::beginLoading() {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
}

void CatalogActivity::checkAndConnectWifi() {
  if (wifiConnected())
    startBrowse();
  else
    launchWifiSelection();
}

void CatalogActivity::launchWifiSelection() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("CAT", "OOM: Wi-Fi selection");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled)
      fail(StrId::STR_WIFI_CONN_FAILED);
    else
      startBrowse();
  });
}

void CatalogActivity::launchSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  if (!keyboard) {
    LOG_ERR("CAT", "OOM: search keyboard");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  state = State::SEARCH_INPUT;
  requestUpdate();
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = State::BROWSING;
    if (!result.isCancelled)
      performSearch(std::get<KeyboardResult>(result.data).text);
    else
      requestUpdate();
  });
}

void CatalogActivity::onSearchEvent(const freeink::ui::ActionEvent&, void* user) {
  auto* self = static_cast<CatalogActivity*>(user);
  if (self->state != State::BROWSING || !self->hasSearch()) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void CatalogActivity::onCancelEvent(const freeink::ui::ActionEvent&, void* user) {
  auto* self = static_cast<CatalogActivity*>(user);
  if (self->state != State::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

bool CatalogActivity::handleCustomInput() {
  if (state == State::WIFI_SELECTION || state == State::SEARCH_INPUT || state == State::DOWNLOADING) return true;
  if (state == State::ERROR) {
    int x = 0, y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      if (wifiConnected())
        retryBrowse();
      else
        launchWifiSelection();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back))
      onBackButton();
    return true;
  }
  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onBackButton();
    return true;
  }
  if (state == State::BROWSING && hasSearch() && nav.selected == 0 &&
      mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    launchSearch();
    return true;
  }
  return false;
}

void CatalogActivity::beginDownload(const std::string& title) {
  state = State::DOWNLOADING;
  statusMessage = title;
  downloadProgress = downloadTotal = 0;
  cancelDownload = goHomeAfterCancel = false;
  requestUpdate(true);
}

void CatalogActivity::onDownloadProgress(const size_t downloaded, const size_t total) {
  downloadProgress = downloaded;
  downloadTotal = total;
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
  if (mappedInput.wasHomeGesture()) cancelDownload = goHomeAfterCancel = true;
  routeTouch(mappedInput);
  const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
  const unsigned long now = millis();
  if (percent >= 100 || lastRenderedPercent < 0 || percent >= lastRenderedPercent + 5 ||
      now - lastProgressUpdateMs >= 5000) {
    lastRenderedPercent = percent;
    lastProgressUpdateMs = now;
    requestUpdate(true);
  }
}

HttpDownloader::DownloadError CatalogActivity::downloadFile(const std::string& url, const std::string& dest,
                                                            const std::string& user, const std::string& password,
                                                            const std::vector<HttpDownloader::Header>& headers) {
  downloadProgress = downloadTotal = 0;
  lastRenderedPercent = -1;
  lastProgressUpdateMs = 0;
  return HttpDownloader::downloadToFile(
      url, dest, [this](size_t downloaded, size_t total) { onDownloadProgress(downloaded, total); }, &cancelDownload,
      user, password, headers);
}

void CatalogActivity::finishDownload(const HttpDownloader::DownloadError result) {
  if (result == HttpDownloader::ABORTED && goHomeAfterCancel) {
    onGoHome();
  } else if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) {
    downloadFinished(result == HttpDownloader::ABORTED);
  } else {
    LOG_ERR("CAT", "Download failed: %d", static_cast<int>(result));
    fail(StrId::STR_DOWNLOAD_FAILED);
  }
}

void CatalogActivity::screenHeader(UiScreen& screen, const char* title) {
  const bool search = state == State::BROWSING && hasSearch();
  catalogScreenHeader(screen, renderer, title,
                      search ? freeink::ui::bitmapFromIcon(icon_search_32) : freeink::ui::BitmapRef{},
                      search ? ACTION_SEARCH : freeink::ui::NO_ACTION);
}

bool CatalogActivity::buildStatusScreen(UiScreen& screen, const bool boldError, const bool showDownloadTotal) {
  switch (state) {
    case State::DOWNLOADING:
      catalogDownloadScreen(screen, statusMessage.c_str(), downloadProgress, showDownloadTotal ? downloadTotal : 0,
                            ACTION_CANCEL);
      break;
    case State::ERROR:
      if (mappedInput.hasTouch()) {
        catalogCenteredBlock(screen, {{tr(STR_ERROR_MSG), boldError}, {errorMessage.c_str()}, {tr(STR_TAP_TO_RETRY)}});
      } else {
        catalogCenteredBlock(screen, {{tr(STR_ERROR_MSG), boldError}, {errorMessage.c_str()}});
      }
      break;
    case State::CHECK_WIFI:
    case State::WIFI_SELECTION:
    case State::SEARCH_INPUT:
    case State::LOADING:
      screen.centeredText(statusMessage.c_str(), screen.theme().bodyText);
      break;
    default:
      return false;
  }
  return true;
}
