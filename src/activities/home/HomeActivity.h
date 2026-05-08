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
  int lastCarouselBookIndex = 0;  // remembered position when leaving carousel row
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;
  bool coverBufferStored = false;
  size_t nextRecentCoverIndex = 0;
  uint8_t* coverBuffer = nullptr;

  std::vector<RecentBook> recentBooks;
  std::vector<MenuEntry> menuEntries;
  bool menuEntriesDirty = true;

  std::string focusBookPath;
  int focusSelectorIndex = -1;

  void onSelectBook(const std::string& path);
  void dispatchMenuAction(MenuAction action);

  void rebuildMenuEntries();
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
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
