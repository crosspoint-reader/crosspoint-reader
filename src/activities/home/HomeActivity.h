#pragma once
#include <cstddef>
#include <functional>
#include <vector>

#include "../Activity.h"
#include "./FileBrowserActivity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
 public:
  enum class MenuAction {
    FileBrowser,
    Recents,
    GlobalBookmarks,
    OpdsBrowser,
    FileTransfer,
    Weather,
    Settings,
  };

 private:
  struct MenuEntry {
    MenuAction action;
    StrId label;
    UIIcon icon;
  };

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  size_t nextRecentCoverIndex = 0;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  std::vector<RecentBook> recentBooks;
  std::vector<MenuEntry> menuEntries;
  bool menuEntriesDirty = true;

  std::string focusBookPath;    // book path to re-select on first render, if present in recents
  int focusSelectorIndex = -1;  // fallback combined-selector index when focusBookPath doesn't match

  void onSelectBook(const std::string& path);
  void dispatchMenuAction(MenuAction action);

  void rebuildMenuEntries();
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string focusBookPath = {},
                        int focusSelectorIndex = -1)
      : Activity("Home", renderer, mappedInput),
        focusBookPath(std::move(focusBookPath)),
        focusSelectorIndex(focusSelectorIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
