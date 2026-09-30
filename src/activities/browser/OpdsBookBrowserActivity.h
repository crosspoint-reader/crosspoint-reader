#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/CatalogActivity.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public CatalogActivity {
 public:
  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;

 private:
  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  // Synthetic pager rows fetchFeed() bracketed the entries with; their footer
  // hint is Fetch (a server round-trip), not Open.
  bool prevRowPresent = false;
  bool nextRowPresent = false;
  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  int listCount() const override { return state == State::BROWSING ? static_cast<int>(entries.size()) : 0; }
  bool hasSearch() const override { return !searchTemplate.empty(); }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void drawFooter() override;
  void buildBrowsingScreen(UiScreen& screen);
  void startBrowse() override;
  void downloadFinished(bool) override { startBrowse(); }
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void onBackButton() override;
  void downloadBook(const OpdsEntry& book);
  void performSearch(const std::string& query) override;
};
