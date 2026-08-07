#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

// One flat list of every book on the card, newest first, with the title on the
// first lines and the author underneath at a fixed column.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows are built only for the visible window, so nothing proportional to the
// library is held: the index streams from SD and the screen keeps at most a
// page of strings.
class LibraryListActivity final : public Activity {
 public:
  explicit LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  bool openIndex();
  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();
  // Close the index, walk the card, reopen and re-seat the screen on what the
  // fresh index holds. The path behind both the Power button and a deletion.
  void rebuildAndReopen();

  // Input
  void handleLetterGridInput();
  bool handleListTouchInput();
  void openSelectedBook();
  void promptDeleteBook();
  void deleteBook(const std::string& path);
  void openSearch();
  void openLetterGrid();
  void applySortOrder(library::SortOrder order);
  void cycleSortOrder(bool forward);
  // Step the sort strip without focusing it: what a held page button does.
  void cycleSortMode(bool forward);
  void nextPage();
  void previousPage(bool selectLast = false);
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();

  // Data
  void applyFilter();
  int rowCount() const;
  int rowFor(int entry) const;
  bool rowTextFor(int entry, std::string& title, std::string& author);
  char letterOf(const library::ClixRecord& record);
  void computeLettersPresent();
  int firstPresentLetter() const;
  void jumpToLetter(char letter);

  // Layout and painting
  void measureRows();
  int rowHeightFor(int titleLines, bool hasAuthor) const;
  void drawSortTabs(int top) const;
  void drawRows();
  void drawLetterGrid() const;
  void drawPositionReadout() const;
  const char* headerTitle() const;

  library::LibraryIndexFile index;
  library::SortOrder sortOrder = library::SortOrder::DateDesc;
  bool indexReady = false;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  int selectedIndex = 0;
  int topIndex = 0;
  int visibleRows = 1;
  // First entry of each page visited on the way here, so going back lands on the
  // same boundaries the reader came through.
  std::vector<uint16_t> pageStarts;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this stays untouched, so the ordinary
  // shelf pays nothing for the feature.
  std::string query;
  std::vector<uint16_t> filtered;

  // Left/Right turn pages, so the sort strip cannot own that axis outright. It
  // takes it only while focused, which the reader reaches by pressing Up from the
  // first book — the one press that had nothing to do before.
  bool tabsFocused = false;
  // Cursor within the strip. Separate from sortOrder because the strip carries
  // one entry that is not a sort mode: Search.
  int tabCursor = 0;

  // The A-Z grid is a mode of this activity, not a separate one: it borrows the
  // same render and input pass, so it needs no lifecycle of its own.
  bool letterGrid = false;
  int letterCursor = 0;
  // One bit per letter, computed when the grid opens. Testing each letter against
  // the index while drawing would re-read every record 26 times per frame.
  uint32_t lettersPresent = 0;
  // Which word of a name the grid's letters refer to. No rule can tell "Qiu
  // Xun" (surname first) from "Jane Austen" (surname last), so the reader
  // says which they mean instead of the code guessing.
  bool jumpByGivenName = false;

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Row geometry captured while building the screen, so input can page by what
  // the last frame actually held and taps can hit rows of unequal height.
  int tabsTop = 0;
  int listTop = 0;
  int listHeight = 0;
  int titleLineH = 0;
  int authorLineH = 0;
  // Top edge of each drawn row plus the bottom edge of the last, so band i is
  // [rowBands[i], rowBands[i + 1]). Sized for the tallest panel this firmware
  // drives at the smallest row height; drawRows stops recording beyond it.
  static constexpr int MAX_VISIBLE_ROWS = 32;
  int16_t rowBands[MAX_VISIBLE_ROWS + 1] = {};
};
