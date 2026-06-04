#pragma once
#include <RssParser.h>

#include <string>
#include <utility>
#include <vector>

#include "RssFeedStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RssFeedBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, ERROR };

  RssFeedBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, RssFeed feed)
      : Activity("RssFeedBrowser", renderer, mappedInput), feed(std::move(feed)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::CHECK_WIFI;
  RssFeed feed;
  std::vector<RssItem> items;
  std::string feedTitle;
  std::string errorMessage;
  std::string statusMessage;
  int selectorIndex = 0;
  bool consumeBack = false;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed();
  void openItem(const RssItem& item);
  bool preventAutoSleep() override { return true; }
};
