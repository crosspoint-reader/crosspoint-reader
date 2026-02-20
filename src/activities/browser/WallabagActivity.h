#pragma once
#include <WallabagClient.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for downloading articles from a Wallabag server.
 * Connects to WiFi, authenticates, fetches article list, and downloads EPUBs
 * to the /Articles/ folder on the SD card.
 */
class WallabagActivity final : public ActivityWithSubactivity {
 public:
  enum class State {
    CHECK_WIFI,      // Checking WiFi connection
    WIFI_SELECTION,  // WiFi selection subactivity is active
    MENU,            // Main menu: Download / Go to Folder
    SYNCING,         // Downloading articles
    DONE,            // Sync complete
    ERROR            // Error state
  };

  explicit WallabagActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome,
                            const std::function<void()>& onGoToArticles)
      : ActivityWithSubactivity("Wallabag", renderer, mappedInput),
        onGoHome(onGoHome),
        onGoToArticles(onGoToArticles) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::CHECK_WIFI;
  int menuIndex = 0;

  std::string statusMessage;
  std::string errorMessage;
  int downloadedCount = 0;
  int totalToDownload = 0;
  int currentArticleNum = 0;

  bool syncPending = false;  // Trigger sync on first loop iteration in SYNCING

  const std::function<void()> onGoHome;
  const std::function<void()> onGoToArticles;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void runSync();
  bool preventAutoSleep() override { return state == State::SYNCING; }
};
