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

  bool handleCustomInput() override;
  bool preventAutoSleep() override { return true; }
  static bool wifiConnected();
  void fail(StrId message);
  void beginLoading();
  void checkAndConnectWifi();
  void launchWifiSelection();
  void launchSearch();
  void beginDownload(const std::string& title);
  void finishDownload(HttpDownloader::DownloadError result);
  HttpDownloader::DownloadError downloadFile(const std::string& url, const std::string& dest,
                                             const std::string& user = {}, const std::string& password = {},
                                             const std::vector<HttpDownloader::Header>& headers = {});
  void screenHeader(UiScreen& screen, const char* title);
  bool buildStatusScreen(UiScreen& screen, bool boldError = true, bool showDownloadTotal = false);

 private:
  static void onSearchEvent(const freeink::ui::ActionEvent&, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent&, void* user);
  void onDownloadProgress(size_t downloaded, size_t total);
  // Download input consumes home gestures before ActivityManager sees them.
  bool goHomeAfterCancel = false;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
};
