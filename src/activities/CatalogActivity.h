#pragma once

#include <I18n.h>

#include "UiListActivity.h"
#include "network/HttpDownloader.h"

// Network catalogs share input, child screens, and transfer lifecycle. Each
// catalog supplies its own feed/auth protocol and navigation destinations.
class CatalogActivity : public UiListActivity {
 public:
  enum class State {
    CHECK_WIFI,
    WIFI_SELECTION,
    SEARCH_INPUT,
    LOADING,
    BROWSING,
    DETAIL,  // publication detail page (OPDS)
    DOWNLOADING,
    ERROR,
    PLUGIN_PICKER,
    LIST_PICKER,
    DONE,
    NO_TOKEN,
    AUTH
  };

  void onEnter() override;
  void onExit() override;

 protected:
  using UiListActivity::UiListActivity;
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_USER;
  static constexpr freeink::ui::ActionId ACTION_CANCEL = ACTION_USER + 1;

  State state = State::LOADING;
  std::string errorMessage, statusMessage;
  size_t downloadProgress = 0, downloadTotal = 0;
  bool cancelDownload = false;

  virtual void startBrowse() = 0;
  virtual void retryBrowse() { startBrowse(); }
  virtual bool hasSearch() const = 0;
  virtual void performSearch(const std::string& query) = 0;
  virtual void downloadFinished(bool cancelled) = 0;
  // Initial keyboard text when search opens (refining the active query).
  virtual std::string searchPrefill() const { return {}; }

  bool handleCustomInput() override;
  void navigateButtons() override;
  // Only work in progress keeps the device awake: a connection, fetch,
  // transfer, sign-in poll, or child flow (Wi-Fi picker, search keyboard).
  // Settled screens (lists, pickers, errors, done, not signed in) may auto-sleep.
  bool preventAutoSleep() override {
    return state == State::CHECK_WIFI || state == State::WIFI_SELECTION || state == State::SEARCH_INPUT ||
           state == State::LOADING || state == State::DOWNLOADING || state == State::AUTH;
  }
  static bool wifiConnected();
  void fail(StrId message);
  void beginLoading();
  void checkAndConnectWifi();
  void launchWifiSelection();
  void launchSearch();
  void beginDownload(const std::string& title);
  void finishDownload(HttpDownloader::DownloadError result);
  // postBody non-null turns the transfer into a POST of that body (e.g. an
  // LCP license to the fulfillment service), with postContentType as its
  // Content-Type. Same heap gating and cancel/progress pump either way.
  HttpDownloader::DownloadError downloadFile(const std::string& url, const std::string& dest,
                                             const std::string& user = {}, const std::string& password = {},
                                             const std::vector<HttpDownloader::Header>& headers = {},
                                             const std::string* postBody = nullptr,
                                             const char* postContentType = nullptr);
  void screenHeader(UiScreen& screen, const char* title);
  bool buildStatusScreen(UiScreen& screen, bool boldError = true, bool showDownloadTotal = false);

 private:
  static void onSearchEvent(const freeink::ui::ActionEvent&, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent&, void* user);
  void onDownloadProgress(size_t downloaded, size_t total);
  // Download input consumes home gestures before ActivityManager sees them.
  bool goHomeAfterCancel = false;
  bool searchPending = false;  // NavPrevious pressed on a searchable top row
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
};
