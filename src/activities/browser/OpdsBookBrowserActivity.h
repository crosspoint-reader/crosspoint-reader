#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsFeedCache.h"
#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 *
 * On enter the cached feed is shown immediately (no WiFi required).
 * The user can then trigger explicit "Update list" or "Sync books" actions,
 * which connect to WiFi and fetch/download as needed.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CACHED,         // Showing feed from disk cache — no WiFi needed
    CHECK_WIFI,     // Checking / waiting for WiFi before a pending action
    WIFI_SELECTION, // Sub-activity: user picking a WiFi network
    LOADING,        // Fetching a feed page from the server
    BROWSING,       // Showing live feed data (sub-catalog navigation)
    SYNCING,        // Bulk-downloading books in the background
    DOWNLOADING,    // Downloading a single book
    ERROR,
    SEARCH_INPUT    // Sub-activity: keyboard for search query
  };

 private:
  enum class PostWifiAction { NONE, FETCH_CURRENT, REFRESH_FEED, SYNC_BOOKS, DOWNLOAD_ENTRY, NAVIGATE_ENTRY };

 public:
  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;         // Copied at construction — safe even if the store changes
  OpdsFeedCache feedCache;   // Disk cache for the root feed
  PostWifiAction pendingWifiAction = PostWifiAction::NONE;
  OpdsEntry pendingEntry;    // Entry to act on once WiFi is ready
  bool fromCache = false;    // True when BROWSING was reached from the CACHED state
  int actionItemCount = 0;   // Number of virtual action items prepended in CACHED mode

  // WiFi / action flow
  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void requireWifi(PostWifiAction action, const OpdsEntry* entry = nullptr);
  void onWifiReady();

  // Feed navigation
  void fetchFeed(const std::string& path, bool returnToCached = false);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();

  // CACHED state helpers
  void buildCachedEntries();

  // Download / open
  void downloadBook(const OpdsEntry& book);
  bool doDownload(const std::string& url, const std::string& filename);
  void syncBooks();
  void openBook(const std::string& path, BrowserState returnState);

  // Book path / download state helpers
  std::string getDownloadedPath(const OpdsEntry& entry) const;
  bool isBookDownloaded(const OpdsEntry& entry) const;
  std::string newPathForBook(const OpdsEntry& entry) const;

  // Search
  void launchSearch();
  void performSearch(const std::string& query);

  bool preventAutoSleep() override { return true; }
};
