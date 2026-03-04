#pragma once
#include <functional>
#include <vector>

#include "../Activity.h"
#include "./MyLibraryActivity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int selectedMenuIndex = 0;
  int selectedBookIndex = 0;
  int menuItemCount = 0;
  int menuOpenBookIndex = -1;
  int menuMyLibraryIndex = -1;
  int menuOpdsIndex = -1;
  int menuTodoIndex = -1;
  int menuAnkiIndex = -1;
  int menuFileTransferIndex = -1;
  int menuSettingsIndex = -1;

  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool inButtonGrid = false;
  bool firstRenderDone = false;
  bool hasOpdsUrl = false;
  bool hasCoverImage = false;
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  bool updateRequired = false;

  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  std::string lastBookTitle;
  std::string lastBookAuthor;
  std::string coverBmpPath;
  std::vector<RecentBook> recentBooks;
  void onContinueReading();
  void onMyLibraryOpen();
  void onNotesOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onTodoOpen();
  void onAnkiOpen();

  void freeCoverBuffer();  // Free the stored cover buffer

 protected:
  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void loadRecentBooks();
  void loadRecentCovers(int coverHeight);
  void openSelectedBook();
  void rebuildMenuLayout();
  bool isPokemonPartyHomeMode() const;
  std::string getMenuItemLabel(int index) const;
  bool drawCoverAt(const std::string& coverPath, int x, int y, int width, int height) const;

  static std::string fallbackTitleFromPath(const std::string& path);
  static std::string fallbackAuthor(const RecentBook& book);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Home", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
