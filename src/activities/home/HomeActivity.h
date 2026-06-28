#pragma once
#include <memory>
#include <vector>

#include "./FileBrowserActivity.h"
#include "./ThemeHomeRenderer.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int coverSelectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;
  bool coverBufferStored = false;
  std::unique_ptr<uint8_t[]> coverBuffer;
  size_t coverBufferSize = 0;
  int coverBufferSelectorIndex = -1;
  bool coverBufferStripSelected = false;
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  std::vector<ThemeHomeActionEntry> homeActions;
  std::vector<int> navigationIndices;
  const HomeMenuItem initialMenuItem;

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();

  void buildHomeActions(std::vector<ThemeHomeActionEntry>& actions) const;
  const std::vector<ThemeHomeActionEntry>& refreshHomeActions();
  int getMenuItemCount();
  static bool storeCoverBufferCallback(void* userData);
  static bool restoreCoverBufferCallback(void* userData);
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(const std::vector<int>& coverHeights);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
