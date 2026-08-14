#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"

// One flat list of every book on the card, newest first, with the title and
// the author underneath.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows render through fui::list on the UiTabListActivity ring (0 = the sort
// strip, 1..N = the books), which is what brings touch: rows, tabs and the A-Z
// grid all register FreeInkUI hit rects. Titles are truncated to one line by
// the widget — more books on the screen, even if half a name is hidden.
//
// Only the visible window of rows is materialized per render (strings and
// ListItems for at most one page), so nothing proportional to the library is
// held: the index streams from SD and the screen keeps a page of strings.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  // Every button is dispatched in handleButtons with this screen's paging
  // semantics; the base ring walk must not run behind it.
  void navigateButtons() override {}

 private:
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_LETTER = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_LETTER_MODE = ACTION_TAB_USER + 1;

  bool openIndex();
  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();

  // Input
  void openSelectedBook();
  void openSearch();
  void openLetterGrid();
  void applySortOrder(library::SortOrder order);
  void cycleSortOrder(bool forward);
  // Swap TitleAsc/TitleDesc in place: the Titles tab keeps its slot and the
  // direction lives on the tab (the drawn triangle), not in a second slot.
  void flipTitleDirection();
  void nextPage();
  void previousPage(bool selectLast = false);
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Touch routing while the grid consumes the loop pass, so its component hit
  // rects still dispatch.
  void routeModalTouch();
  static void letterActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void letterModeActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Data
  void applyFilter();
  int rowCount() const;
  int rowFor(int entry) const;
  bool rowTextFor(int entry, std::string& title, std::string& author);
  char letterOf(const library::ClixRecord& record);
  void computeLettersPresent();
  int firstPresentLetter() const;
  void jumpToLetter(char letter);
  void toggleLetterGridMode();

  // Screen building
  void buildSortTabs(UiScreen& screen);
  int16_t sortStripHeight(UiScreen& screen) const;
  // Materializes ListItems and their strings for the visible window only,
  // mirroring the widget's own layout math (uniform rows, shorter author
  // headings) so the page shown is exactly the page navigation counts.
  void buildRows(UiScreen& screen);
  void buildLetterGrid(UiScreen& screen);
  void drawPositionReadout() const;
  const char* headerTitle() const;

  // Ring 0 is the strip; the selected BOOK is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }

  library::LibraryIndexFile index;
  library::SortOrder sortOrder = library::SortOrder::DateDesc;
  bool indexReady = false;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Cursor within the strip. Separate from the active sort because the strip
  // carries one entry that is not a sort mode: Search.
  int tabCursor = 0;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this stays untouched, so the ordinary
  // shelf pays nothing for the feature.
  std::string query;
  std::vector<uint16_t> filtered;

  // The A-Z grid is a mode of this activity, not a separate one: it borrows the
  // same render and input pass, so it needs no lifecycle of its own.
  bool letterGrid = false;
  int letterCursor = 0;
  // One bit per letter, computed when the grid opens. Testing each letter
  // against the index while drawing would re-read every record 26 times per
  // frame.
  uint32_t lettersPresent = 0;
  // Which word of a name the grid's letters refer to. No rule can tell "Lu
  // Xun" (surname first) from "Jane Austen" (surname last), so the reader says
  // which they mean instead of the code guessing.
  bool jumpByGivenName = false;

  // First entry of each page visited on the way here, so going back lands on
  // the same boundaries the reader came through — pages are not uniform in
  // author order, where section headings consume band height.
  std::vector<uint16_t> pageStarts;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page, never by the library. Headings get their own storage:
  // the surname-first inversion must not overwrite the author slot, whose raw
  // value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;
};
