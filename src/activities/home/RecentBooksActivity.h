#pragma once
#include <I18n.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"

class RecentBooksActivity final : public Activity {
 public:
  static constexpr int GRID_COLS = 3;
  static constexpr int GRID_THUMB_HEIGHT = 160;
  static constexpr int GRID_THUMB_MARGIN = 10;
  static constexpr int GRID_LABEL_HEIGHT = 36;  // two small-font lines below each thumbnail

 private:
  int selectorIndex = 0;
  int initialFocusIndex = -1;  // applied once in onEnter(), then cleared

  std::vector<RecentBook> recentBooks;

  // Lazy cover loading state for grid view
  bool coversLoaded = false;
  bool coversLoading = false;
  bool firstRenderDone = false;
  size_t nextCoverIndex = 0;

  // Partial selection repaint: track previous index so we only redraw two cells
  int prevSelectorIndex = -1;
  bool fullRedrawNeeded = true;

  void loadRecentBooks();
  // Generates the next missing grid thumbnail (one per call). Returns true when all done.
  bool loadNextCover();

  void switchViewMode(bool grid);
  void removeSelectedBook();
  void showSelectedBookInfo();

  // Draws a single grid cell (used for both full render and partial selection update).
  void renderGridCell(int index, bool selected, int cellX, int cellY, int tw, int th, int labelW);

  void renderListView(RenderLock&&);
  void renderGridView(RenderLock&&);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int focusIndex = -1)
      : Activity("RecentBooks", renderer, mappedInput), initialFocusIndex(focusIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
