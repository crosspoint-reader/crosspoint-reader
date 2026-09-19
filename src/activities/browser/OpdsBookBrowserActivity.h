#pragma once
#include <OpdsEntry.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity, private UiAppHost {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  // The active search query. While browsing its results the header shows it
  // (quoted, library-view convention) instead of the server name, and
  // reopening search pre-fills the keyboard with it.
  std::string searchQuery;
  // Quoted form of searchQuery for the header; derived by setSearchQuery().
  std::string headerSearchTitle;
  // searchQuery per navigationHistory entry, pushed/popped in lockstep, so
  // Back restores the search term (or its absence) of the feed it returns to.
  std::vector<std::string> searchQueryHistory;
  // Raw pagination hrefs of the current feed: following them keeps the
  // search-term header (page 2 of results is still the same search).
  std::string pageNextHref;
  std::string pagePrevHref;
  std::string pageFirstHref;
  std::string pageLastHref;
  bool isPaginationHref(const std::string& href) const {
    return (!href.empty()) &&
           (href == pageNextHref || href == pagePrevHref || href == pageFirstHref || href == pageLastHref);
  }
  // Title of the current feed (shown in the header when no search is active).
  std::string feedTitle;
  // "Page X of Y" header subtitle; empty when the feed has no page metadata.
  char pageLabel[48] = {0};
  void setSearchQuery(const std::string& query);
  // Raw search URL template ({searchTerms} or RFC 6570 {?query} style),
  // either inlined in the feed or fetched from an OpenSearch description.
  std::string searchTemplate;
  // OpenSearch description document URL (OPDS 1.x feeds that don't inline a
  // template); fetched lazily on first search.
  std::string searchDescriptionUrl;
  // Base URL the template is relative to: the OpenSearch description URL, or
  // empty when the template came from the feed itself (resolve against feed).
  std::string searchTemplateBase;
  // OAuth access token obtained via the OPDS authentication document's
  // password-grant flow; sent as "Authorization: Bearer" when non-empty.
  std::string bearerToken;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // Viewport memory (top/visibleRows) for the browsing list; `selected` is
  // mirrored from selectorIndex at build/move time.
  freeink::ui::ListNav listNav;
  // Read by HttpDownloader between chunks; set by the Cancel button handler or
  // a Back press, both pumped from the download's progress callback.
  bool cancelDownload = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeAfterCancel = false;

  // Single screen fn dispatching on `state`: every state shares the themed
  // header and gets built through FreeInkUI.
  static void rootScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSearchEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiScreen& screen, bool withSearch);
  void buildBrowsingScreen(UiScreen& screen);
  void buildDownloadScreen(UiScreen& screen);
  void buildStatusScreen(UiScreen& screen);
  void activateSelected();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  bool hasSearch() const { return !searchTemplate.empty() || !searchDescriptionUrl.empty(); }
  bool ensureSearchTemplate();
  bool authenticateWithServer(const std::string& resourceUrl);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
};
