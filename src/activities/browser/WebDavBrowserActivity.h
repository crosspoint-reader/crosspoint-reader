#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "network/WebDavClient.h"
#include "util/ButtonNavigator.h"

class WebDavBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR };

  explicit WebDavBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WebDavBrowser", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<WebDavEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentUrl;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  int failedDownloadIndex = -1;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchListing();
  void navigateToEntry(const WebDavEntry& entry);
  void navigateBack();
  void downloadFile(const WebDavEntry& entry);
  bool preventAutoSleep() override { return true; }

  static std::string formatSize(size_t bytes);
};
