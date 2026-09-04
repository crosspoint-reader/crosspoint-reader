#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "network/OpdsBatchDownload.h"
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
  // A synthetic "Download new" row precedes the feed whenever the current page
  // holds at least one acquisition entry, so rowItems is one longer than
  // entries. Same shape as FontDownloadActivity's "Download All" row.
  bool showDownloadAllRow = false;
  bool hasBookEntry() const;
  int entryIndexFromRow(int rowIndex) const { return showDownloadAllRow ? rowIndex - 1 : rowIndex; }
  bool isDownloadAllRow(const int rowIndex) const { return showDownloadAllRow && rowIndex == 0; }
  int rowCount() const { return static_cast<int>(rowItems.size()); }
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  // Index into rowItems, not entries: a "Download new" row may sit in front of
  // the feed (see entryIndexFromRow()).
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  // Set while "Download new" runs; batchStatus feeds the counter line the
  // download screen adds under the title.
  bool batchActive = false;
  OpdsBatchDownload::Status batchStatus;
  // Repaint throttling across the whole batch (the single-download path keeps
  // its equivalents as locals, one download long).
  int batchRenderedPercent = -1;
  unsigned long batchProgressUpdateMs = 0;

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
  void downloadAllNew();
  static bool onBatchProgress(void* ctx, const OpdsBatchDownload::Status& status);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }
};
