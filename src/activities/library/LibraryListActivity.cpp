#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Rows are drawn by hand rather than through GUI.drawList because that widget
// gives a row one line for its title whenever it also carries a subtitle, which
// makes a wrapped title and an aligned author column mutually exclusive. This
// screen needs both.
constexpr int TITLE_LINES = 3;
// Height of the sort strip. Sized to the small font plus the underline that
// marks the active tab.
constexpr int SIDE_PADDING = 12;
constexpr int ROW_PADDING = 10;
// Hold threshold for the sort menu (firmware convention, cf. FileBrowserActivity).
constexpr unsigned long LONG_PRESS_MS = 1000;

// The strip's tab order, which is also the cycle order.
constexpr library::SortOrder SORT_TABS[] = {library::SortOrder::DateDesc, library::SortOrder::TitleAsc,
                                            library::SortOrder::TitleDesc, library::SortOrder::AuthorAsc};
constexpr int SORT_TAB_COUNT = static_cast<int>(sizeof(SORT_TABS) / sizeof(SORT_TABS[0]));
// One label per SORT_TABS row, same order — the static_assert is what keeps
// the two arrays honest if a sort mode is ever added.
constexpr StrId SORT_MENU_LABELS[] = {StrId::STR_LIBRARY_SORT_RECENT, StrId::STR_LIBRARY_SORT_TITLE_AZ,
                                      StrId::STR_LIBRARY_SORT_TITLE_ZA, StrId::STR_LIBRARY_SORT_AUTHOR};
static_assert(sizeof(SORT_MENU_LABELS) / sizeof(SORT_MENU_LABELS[0]) == sizeof(SORT_TABS) / sizeof(SORT_TABS[0]),
              "every sort mode needs a menu label");
// The strip holds the four sort modes plus Search, which is not one. Moving onto
// a sort mode applies it at once; Search waits for Confirm, since opening a
// keyboard is not something a sideways press should do by itself.
constexpr int SEARCH_TAB = SORT_TAB_COUNT;
constexpr int TAB_SLOTS = SORT_TAB_COUNT + 1;

// 26 letters over 5 columns. A grid rather than a strip because reaching a
// letter costs presses, and each press is a full ~185 ms panel repaint on this
// panel: linear travel averages 13 presses, two dimensions average about 4.5.
constexpr int LETTER_COLS = 5;
constexpr int LETTER_COUNT = 26;

int sortTabIndex(const library::SortOrder order) {
  for (int i = 0; i < SORT_TAB_COUNT; i++) {
    if (SORT_TABS[i] == order) return i;
  }
  return 0;
}

// The strip needs the mode alone. The menu strings carry a "Library ·" prefix
// that reads as four copies of the word once they sit side by side.
const char* sortTabLabel(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::DateDesc:
      return tr(STR_LIBRARY_TAB_RECENT);
    case library::SortOrder::TitleAsc:
      return tr(STR_LIBRARY_TAB_TITLE_AZ);
    case library::SortOrder::TitleDesc:
      return tr(STR_LIBRARY_TAB_TITLE_ZA);
    case library::SortOrder::AuthorAsc:
      return tr(STR_LIBRARY_TAB_AUTHOR);
  }
  return "";
}

}  // namespace

void LibraryListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;

  // Optimistic open: if an index exists, paint from it immediately and let the
  // user decide when to refresh. Only a missing or unreadable index forces the
  // walk, so entering the screen is normally instant.
  indexReady = openIndex();
  if (!indexReady) {
    // Held across the popup and the build, for the same reason the Settings
    // rebuild holds it: the render task's SD-loaded fonts read glyph data at
    // draw time, and the walk needs the card to itself.
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    indexReady = rebuildIndex() && openIndex();
  }
  degraded = indexReady && index.ranksDegraded();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  pageStarts.clear();
  filtered.clear();
  query.clear();
  Activity::onExit();
}

bool LibraryListActivity::openIndex() {
  index.close();
  return index.open(library::libraryIndexPath());
}

bool LibraryListActivity::rebuildIndex() {
  // Carry the monotonic counter forward so "recently added" ordering survives a
  // rebuild: a book that was already on the card must not jump to the top.
  uint16_t carriedFirstSeen = 0;
  {
    library::LibraryIndexFile previous;
    if (previous.open(library::libraryIndexPath())) carriedFirstSeen = previous.header().nextFirstSeen;
  }
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex(
      "/", carriedFirstSeen, stats, SETTINGS.libraryUseMetadata != 0,
      [](const uint16_t booksSoFar, const char*, void*) {
        // Same watchdog feed as the Settings rebuild: the idle task must run
        // or a 5 s panic timeout fires mid-walk.
        if ((booksSoFar & 31u) == 0) delay(1);
        return true;
      },
      nullptr);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped));
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

void LibraryListActivity::openSelectedBook() {
  if (!indexReady) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedIndex)));
  if (ordinal == 0xFFFF) return;

  library::ClixRecord record{};
  std::string path;
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", selectedIndex);
    return;
  }
  // Release the index handle first: on hardware only one reader can hold a file
  // open at a time, and the reader is about to open files of its own.
  index.close();
  indexReady = false;
  onSelectBook(path);
}

// A Confirm hold opens the sort menu. It is a popup rather than a value cycled
// in place, because that is how this codebase changes an enum everywhere else
// (OptionPopup, used by Settings) — and because Left and Right are already
// spent on paging.
void LibraryListActivity::openSortMenu() {
  // Degraded means every order IS discovery order: the strip is hidden, and
  // the hold must not offer a choice that would repaint the same list under a
  // different title.
  if (degraded) return;
  sortPopup.show(StrId::STR_LIBRARY_SORT_TITLE, SORT_MENU_LABELS, SORT_TAB_COUNT, sortTabIndex(sortOrder),
                 [this](const int choice) {
                   if (choice < 0 || choice >= SORT_TAB_COUNT) return;
                   tabCursor = choice;
                   applySortOrder(SORT_TABS[choice]);
                 });
  requestUpdate();
}

void LibraryListActivity::openSearch() {
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query,
                                                                 48, InputType::Text),
                         [this](const ActivityResult& result) {
                           swallowHeldReleases();
                           if (result.isCancelled) return;
                           query = std::get<KeyboardResult>(result.data).text;
                           applyFilter();
                           tabsFocused = false;
                           requestUpdate();
                         });
}

void LibraryListActivity::applySortOrder(const library::SortOrder order) {
  sortOrder = order;
  // A new order invalidates every remembered page boundary: the same ordinal is
  // now somewhere else entirely. It also invalidates the filter, which holds
  // POSITIONS in the old order.
  applyFilter();
  pageStarts.clear();
  selectedIndex = 0;
  topIndex = 0;
  requestUpdate();
}

void LibraryListActivity::cycleSortOrder(const bool forward) {
  tabCursor = (tabCursor + (forward ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  if (tabCursor == SEARCH_TAB) {
    requestUpdate();
    return;
  }
  applySortOrder(SORT_TABS[tabCursor]);
}

int LibraryListActivity::rowCount() const {
  return query.empty() ? static_cast<int>(index.bookCount()) : static_cast<int>(filtered.size());
}

// Entry position on screen to row position in the sort order. Identity while
// unfiltered, so the shelf costs nothing when nothing is typed.
int LibraryListActivity::rowFor(const int entry) const {
  if (query.empty()) return entry;
  if (entry < 0 || entry >= static_cast<int>(filtered.size())) return 0;
  return filtered[entry];
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 512-book cap this is 512 comparisons of at most 96 bytes, which is far below
// the cost of the panel repaint that will follow it anyway.
void LibraryListActivity::applyFilter() {
  filtered.clear();
  // Cleared even on the empty-query path: dropping a filter changes the list just
  // as much as applying one.
  pageStarts.clear();
  if (query.empty()) return;

  // Folded the same way the stored folds were, articles removed included —
  // otherwise "the hobbit" searches for a word no record contains.
  const std::string needle = library::fold(query, /*stripArticle=*/true);
  const int total = static_cast<int>(index.bookCount());
  filtered.reserve(static_cast<size_t>(total));
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      filtered.push_back(static_cast<uint16_t>(row));
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows the
    // author usually also knows where the book is, while "emily" finding Alice
    // Hunter is the case the shelf exists to answer.
    std::string author;
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      filtered.push_back(static_cast<uint16_t>(row));
    }
  }
  selectedIndex = 0;
  topIndex = 0;
}

// Which letter a book files under, matching the column the reader is looking at:
// the title's when sorted by title, the author's when sorted by author. Using the
// title fold in author order sends "Emily Bronte" to wherever her book's title
// happens to fall.
char LibraryListActivity::letterOf(const library::ClixRecord& record) {
  // Must be the key the rows are ORDERED by, not the text they display. The jump
  // scans for the first row at or past the chosen letter, which is only valid
  // while the letters ascend — and the displayed name does not always ascend with
  // the sort. "Hugo Victor" is filed under I, because authorKey sorts a name's
  // words so that "Victor Hugo" and "Hugo Victor" group as one person.
  if (sortOrder == library::SortOrder::AuthorAsc) {
    std::string author;
    if (!index.readAuthor(record, author)) return '\0';
    if (jumpByGivenName) {
      const std::string folded = library::fold(author);
      return folded.empty() ? '\0' : folded[0];
    }
    const std::string key = library::surnameKey(author);
    return key.empty() ? '\0' : key[0];
  }
  return record.foldLen == 0 ? '\0' : record.fold[0];
}

void LibraryListActivity::computeLettersPresent() {
  lettersPresent = 0;
  const int total = rowCount();
  for (int entry = 0; entry < total; entry++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    const char c = letterOf(record);
    if (c >= 'a' && c <= 'z') lettersPresent |= 1u << (c - 'a');
  }
}

int LibraryListActivity::firstPresentLetter() const {
  for (int i = 0; i < LETTER_COUNT; i++) {
    if (lettersPresent & (1u << i)) return i;
  }
  return 0;
}

void LibraryListActivity::openLetterGrid() {
  // Only where an alphabet exists to jump through. Sorted by date there is no
  // letter order to walk, so the press stays inert rather than opening a grid
  // whose every choice would land somewhere arbitrary.
  if (sortOrder == library::SortOrder::DateDesc) return;
  jumpByGivenName = false;
  computeLettersPresent();
  // A shelf of digits or non-Latin titles has no grid to offer; the press
  // stays inert rather than opening an empty screen only Back can leave.
  if (lettersPresent == 0) return;
  letterCursor = firstPresentLetter();
  letterGrid = true;
  requestUpdate();
}

// The fold has already dropped accents and leading articles, so "L'Eneide"
// lands under I and "Éluard" under E — which is what a reader looking under a
// letter expects, and what the raw title would get wrong.
void LibraryListActivity::jumpToLetter(const char letter) {
  const bool descending = sortOrder == library::SortOrder::TitleDesc;
  const int total = rowCount();
  for (int entry = 0; entry < total; entry++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    const char c = letterOf(record);
    // The scan has to follow the direction the shelf runs in. Title Z-A descends,
    // so "at or past" would stop on the very first row every time. Given-name
    // order does not run alphabetically at all — the As are scattered down the
    // whole shelf — so that one matches exactly and lands on the first such book
    // in shelf order.
    const bool hit = jumpByGivenName ? c == letter : descending ? c <= letter : c >= letter;
    if (hit) {
      selectedIndex = entry;
      topIndex = entry;
      pageStarts.clear();
      return;
    }
  }
}

// Title and author for one entry, read straight from the index. Only ever called
// for rows about to be drawn, so at most a screenful of strings exists at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author) {
  title.clear();
  author.clear();
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  std::string name;
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readName(record, name)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) title = name;
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

void LibraryListActivity::loop() {
  if (sortPopup.isActive()) {
    sortPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    // The popup acts on button press; if that button is still held when it
    // closes, swallow its release so it does not also act here.
    if (!sortPopup.isActive()) swallowHeldReleases();
    return;
  }

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return;
  }

  // The grid owns every button while it is open, so its block runs FIRST. Sitting
  // below the Back handlers, its own Back would be unreachable: Back would leave
  // the activity with the grid still on screen.
  if (letterGrid) {
    handleLetterGridInput();
    return;
  }

  // Back clears the filter before it leaves. A shelf showing 7 of 60 books is a
  // state the reader must be able to undo, and giving it the press they would
  // reach for anyway costs no screen space and needs no explaining.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!query.empty()) {
      query.clear();
      filtered.clear();
      selectedIndex = 0;
      topIndex = 0;
      // Boundaries measured against the filtered list mean nothing once the whole
      // shelf is back.
      pageStarts.clear();
      requestUpdate();
      return;
    }
    onGoHome();
    return;
  }

  if (handleListTouchInput()) return;

  const int count = rowCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      openSortMenu();
    } else if (tabsFocused) {
      if (tabCursor == SEARCH_TAB) {
        openSearch();
      } else {
        openLetterGrid();
      }
    } else if (count > 0) {
      openSelectedBook();
    }
    return;
  }

  // Left and Right page. The front pair is the only axis the reader can spare:
  // at 69 books, stepping one row at a time is 34 presses to the middle and
  // paging is 5.
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenRight) && count > 0) {
    if (tabsFocused) {
      cycleSortOrder(/*forward=*/true);
    } else {
      nextPage();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) && count > 0) {
    if (tabsFocused) {
      cycleSortOrder(/*forward=*/false);
    } else {
      previousPage();
    }
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up && count > 0) {
    nextPage();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down && count > 0) {
    previousPage();
    return;
  }

  // The list PAGES, it does not scroll. On e-ink moving one row costs the same
  // full-panel refresh as turning a whole page, so scrolling spends the panel's
  // most expensive operation on its smallest possible result. Up and Down move
  // within the page; at an edge they turn it and land on the far row, so the
  // reader never loses the sense of a fixed frame.
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    if (tabsFocused) {
      // already at the top
    } else if (selectedIndex == 0) {
      // Nothing to focus while the strip is hidden — or drawn above an empty
      // result list, where every tab press would dead-end.
      if (!degraded && count > 0) {
        tabsFocused = true;
        tabCursor = sortTabIndex(sortOrder);
        requestUpdate();
      }
    } else if (selectedIndex > topIndex) {
      selectedIndex--;
      requestUpdate();
    } else if (topIndex > 0) {
      previousPage(/*selectLast=*/true);
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown) && count > 0) {
    if (tabsFocused) {
      tabsFocused = false;
      requestUpdate();
    } else if (selectedIndex < topIndex + visibleRows - 1 && selectedIndex < count - 1) {
      selectedIndex++;
      requestUpdate();
    } else if (topIndex + visibleRows < count) {
      nextPage();
    }
  }
}

void LibraryListActivity::handleLetterGridInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    letterGrid = false;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Refused on a letter no book has. Jumping to where it WOULD fall is a
    // correct answer to a question the reader did not ask.
    if (letterCursor >= 0 && (lettersPresent & (1u << letterCursor))) {
      jumpToLetter(static_cast<char>('a' + letterCursor));
      letterGrid = false;
      // Hand focus back to the list, or the next Confirm reopens the grid
      // instead of opening the book just jumped to.
      tabsFocused = false;
      requestUpdate();
    }
    return;
  }

  // letterCursor == -1 is the mode line above the grid, reached by pressing Up
  // from the top row — the same idiom the sort strip uses, so there is one rule
  // to learn rather than two.
  if (letterCursor < 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) ||
        mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) {
      jumpByGivenName = !jumpByGivenName;
      // The letters present as first names are not those present as surnames.
      // The cursor is on the mode line, not on a letter, so nothing needs
      // re-seating here — Down does that when it enters the grid.
      computeLettersPresent();
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) {
      // Land on a letter that exists. Dropping onto "a" when no book starts with
      // one puts the cursor on a blank cell, which is the state the grid is built
      // to never show.
      letterCursor = firstPresentLetter();
      requestUpdate();
    }
    return;
  }

  int delta = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) delta = 1;
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) delta = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) delta = LETTER_COLS;
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    if (letterCursor < LETTER_COLS) {
      letterCursor = -1;
      requestUpdate();
      return;
    }
    delta = -LETTER_COLS;
  }
  if (delta == 0) return;

  // Skip cells with nothing drawn in them — the cursor must never sit on a blank.
  // Keeping the SAME delta is what makes this safe: stepping by one regardless of
  // direction would make Down walk sideways and the grid stop being
  // two-dimensional. Down still travels a whole row, it just keeps travelling
  // until it finds a letter.
  int next = letterCursor;
  for (int guard = 0; guard < LETTER_COUNT; guard++) {
    next = (next + delta + LETTER_COUNT) % LETTER_COUNT;
    if (lettersPresent & (1u << next)) {
      letterCursor = next;
      break;
    }
  }
  requestUpdate();
}

// Touch is an additive layer over the button flow: a tab tap picks a sort or
// opens the search box, a row tap opens its book. The letter grid stays a
// keyboard affordance — it exists to save presses.
bool LibraryListActivity::handleListTouchInput() {
  // Every hit rect here comes from the last frame's measurements; before the
  // first render there is nothing to test a tap against.
  if (listTop <= 0 || rowCount() == 0) return false;

  if (!degraded) {
    const int slot = renderer.getScreenWidth() / TAB_SLOTS;
    int tab = -1;
    // Must match what drawSortTabs paints: 4 px above the labels, the labels,
    // the underline. A fixed 24 undershoots as soon as the small font scales,
    // and then the bottom of the strip stops accepting taps.
    const int tabsH = renderer.getLineHeight(SMALL_FONT_ID) + 8;
    const auto tabTouch = mappedInput.colTouch(tab, 0, slot, TAB_SLOTS, tabsTop, tabsTop + tabsH, slot);
    if (tabTouch == MappedInputManager::RowTouch::Tap) {
      tabsFocused = false;
      tabCursor = tab;
      if (tab == SEARCH_TAB) {
        openSearch();
      } else {
        applySortOrder(SORT_TABS[tab]);
      }
      return true;
    }
    if (tabTouch != MappedInputManager::RowTouch::None) return true;
  }

  // Rows have content-dependent heights, so the bands recorded by the last frame
  // are the only geometry a tap can be tested against.
  const auto rowAt = [this](const int y) {
    for (int i = 0; i < visibleRows && i < MAX_VISIBLE_ROWS; i++) {
      if (y >= rowBands[i] && y < rowBands[i + 1]) {
        // The bands can be one frame stale: while a search narrows the list,
        // a tap on a row that no longer exists must die here rather than
        // resolve through rowFor()'s zero fallback and open the wrong book.
        const int row = topIndex + i;
        return row < rowCount() ? row : -1;
      }
    }
    return -1;
  };

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int touched = rowAt(ty);
    if (touched >= 0 && touched != selectedIndex) {
      selectedIndex = touched;
      requestUpdate();
    }
    return touched >= 0;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int tapped = rowAt(ty);
    if (tapped < 0) return false;
    selectedIndex = tapped;
    openSelectedBook();
    return true;
  }
  return false;
}

// Height of one row, given how many title lines it actually needs. Rows are
// variable: reserving a second title line for a one-line title leaves a hole
// between the title and its own author, which reads as a layout bug.
//
// The author still sits at a fixed LEFT edge on every row — that is the column
// the eye sweeps. Its vertical position follows its title, which is what makes
// the pair read as one object.
int LibraryListActivity::rowHeightFor(const int titleLines, const bool hasAuthor) const {
  return titleLineH * titleLines + (hasAuthor ? authorLineH : 0) + ROW_PADDING;
}

void LibraryListActivity::measureRows() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  authorLineH = renderer.getLineHeight(SMALL_FONT_ID);
  // The sort strip sits between the header and the list, and takes its height
  // from the list rather than overlaying it.
  tabsTop = metrics.topPadding + metrics.headerHeight;
  listTop = tabsTop + (degraded ? 0 : renderer.getLineHeight(SMALL_FONT_ID) + 8) + metrics.verticalSpacing;
  // The position readout owns the line above the hints, as the file browser's
  // path line does; rows must not be laid out over it.
  const int readoutReserved = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  listHeight = renderer.getScreenHeight() - metrics.buttonHintsHeight - readoutReserved - listTop;
}

// The sort strip: every mode visible at once, the active one underlined. On a
// panel that refreshes whole, showing the alternatives costs nothing per frame
// and saves a menu round-trip to discover them.
void LibraryListActivity::drawSortTabs(const int top) const {
  const int width = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int height = lineH + 8;

  // Equal-width slots, as in the settings tab bar, so the tabs do not shift as
  // labels change.
  const int slot = width / TAB_SLOTS;
  for (int i = 0; i < TAB_SLOTS; i++) {
    const char* label = i == SEARCH_TAB ? tr(STR_LIBRARY_SEARCH) : sortTabLabel(SORT_TABS[i]);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int x = i * slot + (slot - w) / 2;
    // Focused, the cursor marks the pill; unfocused, the active sort does.
    const bool selected = tabsFocused ? i == tabCursor : (i != SEARCH_TAB && SORT_TABS[i] == sortOrder);

    // Focused, the pill inverts, which is the strongest signal this panel has
    // that Left and Right now belong to the strip. Unfocused it stays a plain
    // underline, so the list keeps the reader's attention.
    if (selected && tabsFocused) {
      renderer.fillRoundedRect(x - 6, top + 2, w + 12, height - 4, 4, Color::Black);
    }
    renderer.drawText(SMALL_FONT_ID, x, top + 4, label, !(selected && tabsFocused));
    if (selected && !tabsFocused) renderer.fillRect(x, top + 4 + lineH + 1, w, 1, true);
  }
}

void LibraryListActivity::drawRows() {
  const int count = rowCount();
  if (topIndex > selectedIndex) topIndex = selectedIndex;
  const int width = renderer.getScreenWidth();
  const int textW = width - SIDE_PADDING * 2;

  // Rows have content-dependent heights, so the page is filled by accumulating
  // them rather than by dividing the band. A row is only drawn if it fits whole:
  // a half-drawn row at the bottom edge would look like a rendering fault.
  std::string title;
  std::string author;
  std::string previousAuthor;
  // Sorted by author, the permutation already places one author's books
  // consecutively, so grouping costs one comparison per row and no extra pass.
  // The author then appears once above the run instead of under every title,
  // which is what makes the shelf answer "what else has this person written".
  const bool grouped = sortOrder == library::SortOrder::AuthorAsc;
  // Proximity does the grouping. The heading sits close to the books it names and
  // far from the run above, so it reads as belonging downward; equal gaps on both
  // sides leave it attached to nothing.
  const int groupGapAbove = ROW_PADDING + 6;
  const int groupGapBelow = 3;
  const int groupH = grouped ? authorLineH + groupGapAbove + groupGapBelow : 0;
  // Books indent under their heading, so the left edge itself says which rows
  // belong to whom, without a box or a rule doing the work.
  const int groupIndent = grouped ? 10 : 0;

  int y = listTop;
  int drawn = 0;
  for (int entry = topIndex; entry < count; entry++) {
    rowTextFor(entry, title, author);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, title.c_str(), textW - groupIndent, TITLE_LINES);
    const int height = rowHeightFor(static_cast<int>(lines.size()), !grouped && !author.empty());
    // The first row of a page always carries its heading: without it a page can
    // open on books whose author was named on the page before.
    const bool startsGroup = grouped && !author.empty() && (drawn == 0 || author != previousAuthor);
    previousAuthor = author;
    if (drawn > 0 && y + height + (startsGroup ? groupH : 0) > listTop + listHeight) break;
    if (drawn >= MAX_VISIBLE_ROWS) break;

    if (startsGroup) {
      // Written surname-first, as a catalogue does. The shelf is ORDERED by
      // surname, and printing "Becky Chambers" above a run that sits between
      // Chattam and Melville makes the order look arbitrary — the eye reads B, M, B
      // while the sort follows Ch, Ch, Cr. Only in author order: elsewhere the
      // natural spelling reads better.
      //
      // Into a local, NOT back into `author`: the comparison above reads the next
      // row's author straight from the index, so an overwritten "Xun, Lu"
      // would never match "Lu Xun" and every row would start its own group.
      std::string inverted = author;
      const size_t lastSpace = inverted.find_last_of(' ');
      if (lastSpace != std::string::npos && lastSpace + 1 < inverted.size()) {
        inverted = inverted.substr(lastSpace + 1) + ", " + inverted.substr(0, lastSpace);
      }
      // The first heading on a page needs no gap above it: the strip already
      // bounds the list there.
      const int gap = drawn == 0 ? 2 : groupGapAbove;
      const std::string heading = renderer.truncatedText(SMALL_FONT_ID, inverted.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, SIDE_PADDING, y + gap, heading.c_str(), true);
      y += authorLineH + gap + groupGapBelow;
    }

    rowBands[drawn] = static_cast<int16_t>(y);
    if (entry == selectedIndex) {
      renderer.fillRoundedRect(SIDE_PADDING / 2 + groupIndent, y, width - SIDE_PADDING - groupIndent, height - 2, 6,
                               Color::LightGray);
    }

    int textY = y + ROW_PADDING / 2;
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, SIDE_PADDING + groupIndent, textY, line.c_str(), true);
      textY += titleLineH;
    }
    if (!grouped && !author.empty()) {
      const std::string fitted = renderer.truncatedText(SMALL_FONT_ID, author.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, SIDE_PADDING, textY, fitted.c_str(), true);
    }

    y += height;
    drawn++;
    rowBands[drawn] = static_cast<int16_t>(y);

    // Dotted, not solid: on a 1-bit panel every-other-pixel is how a rule reads
    // grey, and a solid line would outweigh the text it separates. Within a group
    // only — across a boundary the whitespace and the next heading already
    // separate, and a rule there would compete with them.
    bool sameGroup = true;
    if (grouped && entry + 1 < count) {
      std::string nextTitle;
      std::string nextAuthor;
      rowTextFor(entry + 1, nextTitle, nextAuthor);
      sameGroup = nextAuthor == author;
    }
    if (entry + 1 < count && sameGroup && y + ROW_PADDING < listTop + listHeight) {
      for (int x = SIDE_PADDING + groupIndent; x < width - SIDE_PADDING; x += 2) {
        renderer.drawPixel(x, y - 1, true);
      }
    }
  }

  // Report how much this screen held, for the next input pass to page by. Do NOT
  // adjust topIndex here: loop() already scrolled it to contain the selection
  // before asking for this frame, and correcting it afterwards means the frame
  // just drawn can omit the selected row.
  visibleRows = drawn > 0 ? drawn : 1;
  // previousPage() aims past the end because a page's size is only known once it
  // has been measured; clamp now that it has been.
  if (selectedIndex >= topIndex + visibleRows) selectedIndex = topIndex + visibleRows - 1;
  if (selectedIndex >= count) selectedIndex = count - 1;
}

void LibraryListActivity::drawLetterGrid() const {
  const int width = renderer.getScreenWidth();
  const int cell = (width - 2 * SIDE_PADDING) / LETTER_COLS;
  const int rows = (LETTER_COUNT + LETTER_COLS - 1) / LETTER_COLS;
  const int cellH = listHeight / (rows + 1);
  const int top = listTop + cellH / 2;
  // Both modes shown, not just the active one. Printing only the current choice
  // hides the fact that there IS a choice — the same reason the sort strip lists
  // every mode. On a panel that refreshes whole, the second label is free.
  const char* labels[2] = {tr(STR_LIBRARY_JUMP_GIVEN), tr(STR_LIBRARY_JUMP_SURNAME)};
  const int active = jumpByGivenName ? 0 : 1;
  constexpr int gap = 20;
  int labelW[2];
  for (int i = 0; i < 2; i++) labelW[i] = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
  const int modeH = renderer.getLineHeight(SMALL_FONT_ID);
  int mx = (width - (labelW[0] + labelW[1] + gap)) / 2;
  const int modeY = listTop + 2;

  for (int i = 0; i < 2; i++) {
    const bool on = i == active;
    // Focused, the active choice inverts — the strongest signal this panel has
    // that Left and Right are about to change it. Unfocused it keeps an
    // underline, so the line stays quiet while the grid holds attention.
    if (on && letterCursor < 0) {
      renderer.fillRoundedRect(mx - 5, modeY - 2, labelW[i] + 10, modeH + 4, 4, Color::Black);
    }
    renderer.drawText(SMALL_FONT_ID, mx, modeY, labels[i], !(on && letterCursor < 0));
    if (on && letterCursor >= 0) renderer.fillRect(mx, modeY + modeH + 1, labelW[i], 1, true);
    mx += labelW[i] + gap;
  }

  // Centre the block itself. Laying it out from the left margin leaves the last
  // column hanging off the right edge, since 26 letters do not fill 5 columns
  // evenly and the remainder all lands on one side.
  const int originX = (width - LETTER_COLS * cell) / 2;

  for (int i = 0; i < LETTER_COUNT; i++) {
    const int cx = originX + (i % LETTER_COLS) * cell;
    const int cy = top + (i / LETTER_COLS) * cellH;
    // A letter no book starts with is simply not drawn. Its slot stays empty and
    // nothing moves, because the grid's positions come from the alphabet's index
    // and not from what happens to be painted.
    if ((lettersPresent & (1u << i)) == 0) continue;

    // The pill and the glyph share one centre, so the letter sits in the middle
    // of its square rather than in a corner of it.
    const int pillW = cell - 6;
    const int pillH = cellH - 6;
    const int pillX = cx + (cell - pillW) / 2;
    if (i == letterCursor) {
      renderer.fillRoundedRect(pillX, cy, pillW, pillH, 4, Color::Black);
    }
    const char label[2] = {static_cast<char>('A' + i), 0};
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int th = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, pillX + (pillW - tw) / 2, cy + (pillH - th) / 2, label, i != letterCursor);
  }
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. Rows are variable height, so how many fit depends on the
// titles currently on screen — a page total derived from that grows and shrinks
// as you scroll. The book position is stable by construction, and it answers the
// question the reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = rowCount();
  if (count <= 0) return;

  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_POSITION), selectedIndex + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (degraded) return tr(STR_LIBRARY_TITLE_UNSORTED);
  // tr() is a macro that pastes StrId:: onto its argument, so a runtime value
  // has to go through I18N.get directly.
  return I18N.get(SORT_MENU_LABELS[sortTabIndex(sortOrder)]);
}

void LibraryListActivity::render(RenderLock&&) {
  if (sortPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, headerTitle());

  measureRows();
  if (letterGrid) {
    drawLetterGrid();
  } else if (rowCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_LIBRARY_EMPTY));
  } else {
    if (!degraded) drawSortTabs(tabsTop);
    drawRows();
  }

  drawPositionReadout();
  // The front pair carries Left and Right here, not previous and next: it pages
  // the list, switches tabs and steps letters, none of which is one row at a
  // time. mapDirectionalLabels puts each label on whichever button actually
  // carries that screen direction.
  const char* leftLabel = letterGrid || tabsFocused ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV);
  const char* rightLabel = letterGrid || tabsFocused ? tr(STR_DIR_RIGHT) : tr(STR_LIBRARY_PAGE_NEXT);
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), leftLabel, rightLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// Page boundaries are content-dependent, so they cannot be computed from an
// index — a page holds as many rows as its own titles allow. They are therefore
// remembered as the reader moves forward, which makes going back exact rather
// than an estimate that would drift on every turn.
void LibraryListActivity::nextPage() {
  const int count = rowCount();
  const int next = topIndex + visibleRows;
  if (next >= count) return;
  if (pageStarts.empty()) pageStarts.push_back(0);
  pageStarts.push_back(static_cast<uint16_t>(next));
  topIndex = next;
  selectedIndex = topIndex;
  requestUpdate();
}

void LibraryListActivity::previousPage(const bool selectLast) {
  if (topIndex <= 0) return;
  if (pageStarts.size() > 1) {
    pageStarts.pop_back();
    topIndex = pageStarts.back();
  } else {
    // No recorded history — the reader jumped here by some other route. Fall back
    // to a screenful back; it may not land on a boundary this pass, but the next
    // render re-measures and nothing is lost.
    topIndex = std::max(0, topIndex - visibleRows);
    pageStarts.assign(1, static_cast<uint16_t>(topIndex));
  }
  // selectLast is only known to be right after the render that measures this
  // page, so aim past the end and let drawRows clamp it.
  selectedIndex = selectLast ? topIndex + visibleRows - 1 : topIndex;
  requestUpdate();
}
