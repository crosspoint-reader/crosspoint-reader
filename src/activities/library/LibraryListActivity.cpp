#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
// Hold threshold for the strip's secondary actions (firmware convention, cf.
// FileBrowserActivity).
constexpr unsigned long LONG_PRESS_MS = 1000;

// Which way the Titles tab runs. One direction for the whole power-on session:
// it survives this activity's delete-on-exit on purpose, and it is deliberately
// not a setting — a preference the next boot can simply re-express in one hold.
bool sTitleDescending = false;

// The strip's tab order, which is also the cycle order. Both title directions
// live on ONE tab: Z-A paid a full strip slot for a rare use, and direction is
// state on the tab (the drawn triangle), not a place in the row. Search is not
// a sort mode: moving onto a sort tab applies it at once; Search waits for
// Confirm, since opening a keyboard is not something a sideways press should
// do by itself.
constexpr int RECENT_TAB = 0;
constexpr int TITLES_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int SEARCH_TAB = 3;
constexpr int TAB_SLOTS = SEARCH_TAB + 1;

// No longer one-to-one: both title orders map to the Titles tab.
int sortTabIndex(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::TitleAsc:
    case library::SortOrder::TitleDesc:
      return TITLES_TAB;
    case library::SortOrder::AuthorAsc:
      return AUTHOR_TAB;
    case library::SortOrder::DateDesc:
      return RECENT_TAB;
  }
  return RECENT_TAB;
}

library::SortOrder orderForTab(const int tab) {
  if (tab == TITLES_TAB) return sTitleDescending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return library::SortOrder::AuthorAsc;
  return library::SortOrder::DateDesc;
}

// The strip needs the mode alone. The header strings carry a "Library ·"
// prefix that reads as four copies of the word once they sit side by side.
const char* tabLabelFor(const int tab) {
  if (tab == TITLES_TAB) return tr(STR_LIBRARY_TAB_TITLES);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  if (tab == SEARCH_TAB) return tr(STR_LIBRARY_SEARCH);
  return tr(STR_LIBRARY_TAB_RECENT);
}

// 26 letters over 5 columns. A grid rather than a strip because reaching a
// letter costs presses, and each press is a full ~185 ms panel repaint on this
// panel: linear travel averages 13 presses, two dimensions average about 4.5.
constexpr int LETTER_COLS = 5;
constexpr int LETTER_COUNT = 26;

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput) {}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_LETTER, &LibraryListActivity::letterActionTrampoline, this);
  app.on(ACTION_LETTER_MODE, &LibraryListActivity::letterModeActionTrampoline, this);
  tabCursor = sortTabIndex(sortOrder);
  auto& nav = activeNav();
  nav.selected = 1;
  nav.top = 0;

  // Optimistic open: if an index exists, paint from it immediately and let the
  // user decide when to refresh. Only a missing or unreadable index forces the
  // walk, so entering the screen is normally instant.
  if (!index.open(library::libraryIndexPath())) {
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    if (rebuildIndex()) index.open(library::libraryIndexPath());
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

void LibraryListActivity::openSelectedBook() {
  if (!index.isOpen()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
  if (ordinal == 0xFFFF) return;

  library::ClixRecord record{};
  std::string path;
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
    return;
  }
  // The reader screen this opens has its own surfaces; a lingering tap flash
  // would gray an unrelated element there.
  app.clearTapFlash();
  // Release the index handle first: on hardware only one reader can hold a file
  // open at a time, and the reader is about to open files of its own.
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(int) { openSelectedBook(); }

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
                           auto& nav = activeNav();
                           if (!query.empty() && filteredCount == 0 && !degraded) {
                             // Keep Search reachable when nothing matches. A
                             // row cursor at 1 would point at no component and
                             // trap Left/Right behind count == 0.
                             nav.selected = 0;
                             tabCursor = SEARCH_TAB;
                           } else {
                             // A non-empty result belongs to the list: land on
                             // its first surviving row, not on the strip.
                             nav.selected = 1;
                           }
                           nav.top = 0;
                           requestUpdate();
                         });
}

void LibraryListActivity::applySortOrder(const library::SortOrder order) {
  sortOrder = order;
  // The filter holds positions in the old order, so it must be rebuilt.
  applyFilter();
  // The order changed under the ring; the strip keeps the focus it had, any
  // row selection collapses to the first row of the new order.
  auto& nav = activeNav();
  if (nav.selected != 0) nav.selected = 1;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::cycleSortOrder(const bool forward) {
  tabCursor = (tabCursor + (forward ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  if (tabCursor == SEARCH_TAB) {
    requestUpdate();
    return;
  }
  applySortOrder(orderForTab(tabCursor));
}

void LibraryListActivity::stepTab(const int direction) { cycleSortOrder(direction > 0); }

// Direction is a flip of existing state, not a different tab: the strip keeps
// its width and the reader keeps their place in the row of tabs. The header
// keeps announcing the full truth ("Library · Title Z-A").
void LibraryListActivity::flipTitleDirection() {
  sTitleDescending = !sTitleDescending;
  if (sortTabIndex(sortOrder) == TITLES_TAB) {
    applySortOrder(orderForTab(TITLES_TAB));
    requestUpdate(true);
    return;
  }
  requestUpdate();
}

// Touch tap on a strip pill: the same application the button cycle does, minus
// the travel. Taps land with the tab bar focused, like the button cycle
// leaves it.
void LibraryListActivity::onTabAction(const int index) {
  tabCursor = index;
  if (index == SEARCH_TAB) {
    app.clearTapFlash();
    openSearch();
    return;
  }
  app.clearTapFlash();
  applySortOrder(orderForTab(index));
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
}

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

// The ACTIVE sort's tab — never Search: Search is an action on the strip, not
// a sort, so the strip's own cursor is the only state that can sit on it.
int LibraryListActivity::activeTab() const { return sortTabIndex(sortOrder); }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

int LibraryListActivity::rowCount() const {
  return query.empty() ? static_cast<int>(index.bookCount()) : static_cast<int>(filteredCount);
}

int LibraryListActivity::listCount() const { return rowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered, so the shelf costs nothing when nothing is typed.
int LibraryListActivity::rowFor(const int entry) const {
  if (query.empty()) return entry;
  if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
  return filtered[entry];
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  if (query.empty()) return;

  // Folded the same way the stored folds were, articles removed included —
  // otherwise "the hobbit" searches for a word no record contains.
  const std::string needle = library::fold(query, /*stripArticle=*/true);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Which letter a book files under, matching the column the reader is looking at:
// the title's when sorted by title, the author's when sorted by author. Using
// the title fold in author order sends "Emily Bronte" to wherever her book's
// title happens to fall.
char LibraryListActivity::letterOf(const library::ClixRecord& record) {
  // Must be the key the rows are ORDERED by, not the text they display. The
  // jump scans for the first row at or past the chosen letter, which is only
  // valid while the letters ascend — and the displayed name does not always
  // ascend with the sort. "Hugo Victor" is filed under I, because authorKey
  // sorts a name's words so that "Victor Hugo" and "Hugo Victor" group as one
  // person.
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
    // The scan has to follow the direction the shelf runs in. Title Z-A
    // descends, so "at or past" would stop on the very first row every time.
    // Given-name order does not run alphabetically at all — the As are
    // scattered down the whole shelf — so that one matches exactly and lands on
    // the first such book in shelf order.
    const bool hit = jumpByGivenName ? c == letter : descending ? c <= letter : c >= letter;
    if (hit) {
      auto& nav = activeNav();
      nav.selected = entry + 1;
      nav.top = entry;
      return;
    }
  }
}

// The mode line above the grid. Sorted by author the choice is which WORD the
// letters mean; sorted by title it is the direction — the novice path to the
// same flip the strip's hold offers.
void LibraryListActivity::toggleLetterGridMode() {
  if (sortOrder == library::SortOrder::AuthorAsc) {
    jumpByGivenName = !jumpByGivenName;
    // The letters present as first names are not those present as surnames.
    // The cursor is on the mode line, not on a letter, so nothing needs
    // re-seating here — Down does that when it enters the grid.
    computeLettersPresent();
  } else {
    // The letter SET is direction blind (first letters do not change), so
    // nothing needs recomputing.
    flipTitleDirection();
  }
  requestUpdate();
}

void LibraryListActivity::letterActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (!self->letterGrid) return;
  if (event.value < 0 || event.value >= LETTER_COUNT) return;
  if (!(self->lettersPresent & (1u << event.value))) return;
  self->jumpToLetter(static_cast<char>('a' + event.value));
  self->letterGrid = false;
  self->app.clearTapFlash();
  self->requestUpdate();
}

void LibraryListActivity::letterModeActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (!self->letterGrid) return;
  // Tapping the already-active choice states nothing new.
  const bool titleOrder = self->sortOrder != library::SortOrder::AuthorAsc;
  const int active = titleOrder ? (sTitleDescending ? 1 : 0) : (self->jumpByGivenName ? 0 : 1);
  if (event.value == active) return;
  self->toggleLetterGridMode();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
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

bool LibraryListActivity::handleCustomInput() {
  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  // The grid owns every button while it is open, so its block runs FIRST.
  // Sitting below the Back handlers, its own Back would be unreachable: Back
  // would leave the activity with the grid still on screen.
  if (letterGrid) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      letterGrid = false;
      requestUpdate();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Refused on a letter no book has. Jumping to where it WOULD fall is a
      // correct answer to a question the reader did not ask.
      if (letterCursor >= 0 && (lettersPresent & (1u << letterCursor))) {
        jumpToLetter(static_cast<char>('a' + letterCursor));
        letterGrid = false;
        requestUpdate();
      }
      return true;
    }

    // letterCursor == -1 is the mode line above the grid, reached by pressing
    // Up from the top row — the same idiom the sort strip uses, so there is one
    // rule to learn rather than two.
    if (letterCursor < 0) {
      if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) ||
          mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) {
        toggleLetterGridMode();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) {
        // Land on a letter that exists. Dropping onto "a" when no book starts
        // with one puts the cursor on a blank cell, which is the state the grid
        // is built to never show.
        letterCursor = firstPresentLetter();
        requestUpdate();
      }
      routeModalTouch();
      return true;
    }

    int delta = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) delta = 1;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) delta = -1;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) delta = LETTER_COLS;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
      if (letterCursor < LETTER_COLS) {
        letterCursor = -1;
        requestUpdate();
        return true;
      }
      delta = -LETTER_COLS;
    }
    if (delta != 0) {
      // Skip cells with nothing drawn in them — the cursor must never sit on a
      // blank. Keeping the SAME delta is what makes this safe: stepping by one
      // regardless of direction would make Down walk sideways and the grid stop
      // being two-dimensional. Down still travels a whole row, it just keeps
      // travelling until it finds a letter.
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
    routeModalTouch();
    return true;
  }

  // Swipes page like the front pair: page boundaries stay exact, and the
  // remembered path they walk stays valid.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up && rowCount() > 0) {
    nextPage();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Down && rowCount() > 0) {
    previousPage();
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = rowCount();
  auto& nav = activeNav();

  // Back clears the filter before it leaves. A shelf showing 7 of 60 books is
  // a state the reader must be able to undo, and giving it the press they
  // would reach for anyway costs no screen space and needs no explaining.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!query.empty()) {
      query.clear();
      applyFilter();
      if (nav.selected != 0) nav.selected = 1;
      nav.top = 0;
      requestUpdate();
    } else {
      onGoHome();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Dispatched on release, by held time, so one press means exactly one
    // thing. A hold is the secondary action of the FOCUSED context.
    if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      if (tabsFocused()) {
        // On the Titles tab the hold flips the direction — the triangle and
        // the header both change, so the flip explains itself. On Search it
        // clears the filter without a trip through the keyboard.
        if (tabCursor == TITLES_TAB) flipTitleDirection();
        if (tabCursor == SEARCH_TAB && !query.empty()) {
          query.clear();
          applyFilter();
          if (nav.selected != 0) nav.selected = 1;
          nav.top = 0;
          requestUpdate(true);
        }
      }
      return true;
    }
    if (tabsFocused()) {
      if (tabCursor == SEARCH_TAB) {
        openSearch();
      } else {
        openLetterGrid();
      }
      return true;
    }
    if (count > 0) openSelectedBook();
    return true;
  }

  // Left and Right page. The front pair is the only axis the reader can spare:
  // at 69 books, stepping one row at a time is 34 presses to the middle and
  // paging is 5.
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenRight) && (tabsFocused() || count > 0)) {
    if (tabsFocused()) {
      cycleSortOrder(/*forward=*/true);
    } else {
      nextPage();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) && (tabsFocused() || count > 0)) {
    if (tabsFocused()) {
      cycleSortOrder(/*forward=*/false);
    } else {
      previousPage();
    }
    return true;
  }

  // The list PAGES, it does not scroll. On e-ink moving one row costs the same
  // full-panel refresh as turning a whole page, so scrolling spends the panel's
  // most expensive operation on its smallest possible result. Up and Down move
  // within the page; at an edge they turn it and land on the far row, so the
  // reader never loses the sense of a fixed frame.
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) {
    if (tabsFocused()) {
      // already at the top
    } else if (nav.selected == 1) {
      // Nothing to focus while the strip is hidden — or drawn above an empty
      // result list, where every tab press would dead-end.
      if (!degraded && count > 0) {
        nav.selected = 0;
        tabCursor = sortTabIndex(sortOrder);
        requestUpdate();
      }
    } else if (nav.selected - 1 > nav.top) {
      nav.selected--;
      requestUpdate();
    } else if (nav.top > 0) {
      previousPage();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown) && count > 0) {
    if (tabsFocused()) {
      nav.selected = 1;
      requestUpdate();
    } else if (nav.selected - 1 < nav.top + nav.pageRows() - 1 && nav.selected - 1 < count - 1) {
      nav.selected++;
      requestUpdate();
    } else if (nav.top + nav.pageRows() < count) {
      nextPage();
    }
    return true;
  }

  return false;
}

// Touch routing while the grid consumes the loop pass: the base's routing
// never runs there, so the app's hit rects (letters, mode line) are routed
// here instead.
void LibraryListActivity::routeModalTouch() {
  const auto route = UiAppHost::routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
}

// The strip takes its height from its own label line: a fixed band clips the
// labels as soon as the small font grows. The grid consumes the same height so
// every mode's content starts at the same y.
int16_t LibraryListActivity::sortStripHeight(UiScreen& screen) const {
  return static_cast<int16_t>(screen.target().lineHeight(screen.theme().smallText.font) + 8);
}

// The sort strip: every mode visible at once, the active one underlined. On a
// panel that refreshes whole, showing the alternatives costs nothing per frame
// and saves a menu round-trip to discover them.
//
// The band is a fui::tabBar — which is what makes the pills tappable — with a
// two-state treatment: focused, the cursor's pill inverts (the strongest
// signal this panel has that Left and Right now belong to the strip);
// unfocused, the active sort carries a plain underline, so the list keeps the
// reader's attention.
void LibraryListActivity::buildSortTabs(UiScreen& screen) {
  fui::TabItem tabs[TAB_SLOTS];
  for (int i = 0; i < TAB_SLOTS; i++) {
    tabs[i].label = tabLabelFor(i);
    tabs[i].value = static_cast<int16_t>(i);
    // Focused, the cursor marks the pill; unfocused, the active sort does.
    tabs[i].selected = tabsFocused() ? i == tabCursor : (i != SEARCH_TAB && sortTabIndex(sortOrder) == i);
  }

  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = TAB_SLOTS;
  props.action = ACTION_TAB;
  props.inputMask = fui::InputTouch;
  props.text = screen.theme().smallText;
  props.tabInset = fui::Insets{2, 0, 2, 0};
  props.contentInset = fui::Insets{2, 6, 2, 6};
  props.minTouchSize = screen.theme().minTouchSize;

  fui::StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (tabsFocused()) {
    styles.selected.background = fui::Paint::solid(fui::Color::Black);
    styles.selected.foreground = fui::Paint::solid(fui::Color::White);
    styles.selected.radius = 4;
  } else {
    styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    props.selectedUnderline = 1;
  }
  styles.focused = styles.selected;
  styles.active = styles.selected;
  props.tabStyles = styles;

  const fui::Rect band = screen.takeTop(sortStripHeight(screen));
  fui::tabBar(screen.frame(), band, props);

  // The two state decorations no component slot carries, drawn on the band
  // through the same target: the active Titles tab tells its direction as a
  // small triangle, so direction is never hidden state; an active query
  // filters every sort, so its tab says so with a dot.
  const int16_t slot = static_cast<int16_t>(band.width / TAB_SLOTS);
  const int16_t lineH = screen.target().lineHeight(props.text.font);
  if (sortTabIndex(sortOrder) == TITLES_TAB) {
    const int16_t w = screen.target().measureText(props.text.font, tabLabelFor(TITLES_TAB), props.text).width;
    const int16_t x = static_cast<int16_t>(band.x + TITLES_TAB * slot + (slot - w) / 2 + w + 5);
    const int16_t midY = static_cast<int16_t>(band.y + 2 + (lineH + 4) / 2);
    constexpr int16_t triW = 7;
    constexpr int16_t triH = 4;
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    if (sTitleDescending) {
      screen.target().triangle(fui::Point{x, static_cast<int16_t>(midY - triH / 2)},
                               fui::Point{static_cast<int16_t>(x + triW), static_cast<int16_t>(midY - triH / 2)},
                               fui::Point{static_cast<int16_t>(x + triW / 2), static_cast<int16_t>(midY + triH / 2)},
                               ink);
    } else {
      screen.target().triangle(fui::Point{static_cast<int16_t>(x + triW / 2), static_cast<int16_t>(midY - triH / 2)},
                               fui::Point{x, static_cast<int16_t>(midY + triH / 2)},
                               fui::Point{static_cast<int16_t>(x + triW), static_cast<int16_t>(midY + triH / 2)}, ink);
    }
  }
  if (!query.empty()) {
    const int16_t w = screen.target().measureText(props.text.font, tabLabelFor(SEARCH_TAB), props.text).width;
    const int16_t x = static_cast<int16_t>(band.x + SEARCH_TAB * slot + (slot - w) / 2 + w + 5);
    constexpr int16_t dotW = 5;
    const int16_t y = static_cast<int16_t>(band.y + 2 + (lineH + 4 - dotW) / 2);
    screen.target().fill(fui::Rect{x, y, dotW, dotW}, fui::Paint::solid(fui::Color::Black), 2);
  }
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = rowCount();
  const bool grouped = sortOrder == library::SortOrder::AuthorAsc;

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.headerUnderline = false;
  props.scrollIndicator = false;
  syncTabListViewport(screen, props, /*hasSubtitle=*/!grouped);

  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1);
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int books = 0;
  int headers = 0;
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && books < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(books)];
    std::string& author = winAuthors[static_cast<size_t>(books)];
    rowTextFor(entry, title, author);

    const bool startsGroup = grouped && (books == 0 || author != winAuthors[static_cast<size_t>(books - 1)]);
    fui::ListItem item;
    if (startsGroup) {
      std::string& heading = winHeaders[static_cast<size_t>(headers++)];
      heading = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
      if (!author.empty()) {
        const size_t lastSpace = heading.find_last_of(' ');
        if (lastSpace != std::string::npos && lastSpace + 1 < heading.size()) {
          heading = heading.substr(lastSpace + 1) + ", " + heading.substr(0, lastSpace);
        }
      }
      item.sectionHeading = heading.c_str();
    }

    item.label = title.c_str();
    if (!grouped && !author.empty()) item.subtitle = author.c_str();
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    books++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
}

// The A-Z grid, as components: one button per PRESENT letter (an absent letter
// is simply not drawn — its slot stays empty and nothing moves, because the
// grid's positions come from the alphabet's index), and two mode buttons above
// it. Buttons are what make the letters tappable.
void LibraryListActivity::buildLetterGrid(UiScreen& screen) {
  const fui::Rect body = screen.body();
  auto& target = screen.target();
  const int cell = (body.width - 2 * SIDE_PADDING) / LETTER_COLS;
  const int rows = (LETTER_COUNT + LETTER_COLS - 1) / LETTER_COLS;
  const int cellH = body.height / (rows + 1);

  // Both modes shown, not just the active one. Printing only the current
  // choice hides the fact that there IS a choice — the same reason the sort
  // strip lists every mode. Sorted by author the choice is which WORD the
  // letters mean; sorted by title it is the direction. "A-Z"/"Z-A" are letter
  // symbols rather than words, so they carry no translation.
  const bool titleOrder = sortOrder != library::SortOrder::AuthorAsc;
  const char* labels[2] = {titleOrder ? "A-Z" : tr(STR_LIBRARY_JUMP_GIVEN),
                           titleOrder ? "Z-A" : tr(STR_LIBRARY_JUMP_SURNAME)};
  const int active = titleOrder ? (sTitleDescending ? 1 : 0) : (jumpByGivenName ? 0 : 1);
  // Centered — and deliberately NOT all-default: screen.button() substitutes
  // the body font for any style that fails textStyleUnset, and these labels
  // are measured in the small font.
  fui::TextStyle modeText = screen.theme().smallText;
  modeText.align = fui::TextAlign::Center;
  const int16_t modeH = target.lineHeight(modeText.font);
  constexpr int16_t MODE_GAP = 20;
  int16_t labelW[2];
  for (int i = 0; i < 2; i++) labelW[i] = target.measureText(modeText.font, labels[i], modeText).width;
  int16_t mx = static_cast<int16_t>(body.x + (body.width - (labelW[0] + labelW[1] + MODE_GAP)) / 2);
  const int16_t modeY = static_cast<int16_t>(body.y + 2);

  for (int i = 0; i < 2; i++) {
    // Focused, the active choice inverts — the strongest signal this panel has
    // that Left and Right are about to change it. Unfocused it keeps an
    // underline, so the line stays quiet while the grid holds attention.
    const bool on = i == active;
    fui::ButtonProps mode;
    mode.label = labels[i];
    mode.action = ACTION_LETTER_MODE;
    mode.value = static_cast<int16_t>(i);
    mode.text = modeText;
    mode.styles.explicitlySet = true;
    mode.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    if (on && letterCursor < 0) {
      mode.styles.normal.background = fui::Paint::solid(fui::Color::Black);
      mode.styles.normal.foreground = fui::Paint::solid(fui::Color::White);
      mode.styles.normal.radius = 4;
    }
    screen.button(mode, fui::Rect{static_cast<int16_t>(mx - 5), static_cast<int16_t>(modeY - 2),
                                  static_cast<int16_t>(labelW[i] + 10), static_cast<int16_t>(modeH + 4)});
    if (on && letterCursor >= 0) {
      target.fill(fui::Rect{mx, static_cast<int16_t>(modeY + modeH + 1), labelW[i], 1},
                  fui::Paint::solid(fui::Color::Black));
    }
    mx = static_cast<int16_t>(mx + labelW[i] + MODE_GAP);
  }

  // Centre the block itself: 26 letters do not fill 5 columns evenly, and laid
  // out from the left margin the remainder all lands on one side.
  const int16_t top = static_cast<int16_t>(body.y + cellH / 2);
  const int16_t originX = static_cast<int16_t>(body.x + (body.width - LETTER_COLS * cell) / 2);
  char letterLabels[LETTER_COUNT][2];

  for (int i = 0; i < LETTER_COUNT; i++) {
    if (!(lettersPresent & (1u << i))) continue;
    const int16_t cx = static_cast<int16_t>(originX + (i % LETTER_COLS) * cell);
    const int16_t cy = static_cast<int16_t>(top + (i / LETTER_COLS) * cellH);
    const int16_t pillW = static_cast<int16_t>(cell - 6);
    const int16_t pillH = static_cast<int16_t>(cellH - 6);
    letterLabels[i][0] = static_cast<char>('A' + i);
    letterLabels[i][1] = '\0';
    fui::ButtonProps key;
    key.label = letterLabels[i];
    key.action = ACTION_LETTER;
    key.value = static_cast<int16_t>(i);
    key.text = screen.theme().bodyText;
    key.styles.explicitlySet = true;
    key.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    if (i == letterCursor) {
      key.styles.normal.background = fui::Paint::solid(fui::Color::Black);
      key.styles.normal.foreground = fui::Paint::solid(fui::Color::White);
      key.styles.normal.radius = 4;
    }
    screen.button(key, fui::Rect{static_cast<int16_t>(cx + (cell - pillW) / 2), cy, pillW, pillH});
  }
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band, above the button hints. The position
  // readout owns the line above the hints, as the file browser's path line
  // does; rows must not be laid out over it.
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});

  if (letterGrid) {
    screen.spacer(static_cast<int16_t>(sortStripHeight(screen) + metrics.verticalSpacing));
    buildLetterGrid(screen);
    return;
  }

  if (!degraded) {
    buildSortTabs(screen);
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }
  if (rowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = rowCount();
  if (count <= 0) return;

  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_POSITION), selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (degraded) return tr(STR_LIBRARY_TITLE_UNSORTED);
  switch (sortOrder) {
    case library::SortOrder::TitleAsc:
      return tr(STR_LIBRARY_SORT_TITLE_AZ);
    case library::SortOrder::TitleDesc:
      return tr(STR_LIBRARY_SORT_TITLE_ZA);
    case library::SortOrder::AuthorAsc:
      return tr(STR_LIBRARY_SORT_AUTHOR);
    case library::SortOrder::DateDesc:
      return tr(STR_LIBRARY_SORT_RECENT);
  }
  return tr(STR_LIBRARY_SORT_RECENT);
}

void LibraryListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, headerTitle());

  renderUi();

  drawPositionReadout();
  // The front pair carries Left and Right here, not previous and next: it pages
  // the list, switches tabs and steps letters, none of which is one row at a
  // time. mapDirectionalLabels puts each label on whichever button actually
  // carries that screen direction.
  const char* leftLabel = letterGrid || tabsFocused() ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV);
  const char* rightLabel = letterGrid || tabsFocused() ? tr(STR_DIR_RIGHT) : tr(STR_LIBRARY_PAGE_NEXT);
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), leftLabel, rightLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void LibraryListActivity::nextPage() {
  const int count = rowCount();
  auto& nav = activeNav();
  const int next = nav.top + std::max(1, nav.pageRows());
  if (next >= count) return;
  nav.top = next;
  nav.selected = next + 1;
  requestUpdate();
}

void LibraryListActivity::previousPage() {
  auto& nav = activeNav();
  if (nav.top <= 0) return;
  nav.top = std::max(0, nav.top - std::max(1, nav.pageRows()));
  nav.selected = nav.top + 1;
  requestUpdate();
}
