#include "LibraryListActivity.h"

#include <FreeInkUIIcon.h>
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
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/search32.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;

// One tab per sort order keeps the shared tab bar authoritative: selecting a
// tab fully describes the ordering, with no second direction state.
constexpr int RECENT_TAB = 0;
constexpr int TITLE_ASC_TAB = 1;
constexpr int TITLE_DESC_TAB = 2;
constexpr int AUTHOR_TAB = 3;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

int sortTabIndex(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::TitleAsc:
      return TITLE_ASC_TAB;
    case library::SortOrder::TitleDesc:
      return TITLE_DESC_TAB;
    case library::SortOrder::AuthorAsc:
      return AUTHOR_TAB;
    case library::SortOrder::DateDesc:
      return RECENT_TAB;
  }
  return RECENT_TAB;
}

library::SortOrder orderForTab(const int tab) {
  if (tab == TITLE_ASC_TAB) return library::SortOrder::TitleAsc;
  if (tab == TITLE_DESC_TAB) return library::SortOrder::TitleDesc;
  if (tab == AUTHOR_TAB) return library::SortOrder::AuthorAsc;
  return library::SortOrder::DateDesc;
}

// The strip needs the mode alone. The header strings carry a "Library ·"
// prefix that reads as four copies of the word once they sit side by side.
const char* tabLabelFor(const int tab) {
  if (tab == TITLE_ASC_TAB) return tr(STR_LIBRARY_TAB_TITLE_AZ);
  if (tab == TITLE_DESC_TAB) return tr(STR_LIBRARY_TAB_TITLE_ZA);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
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
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_LETTER, &LibraryListActivity::letterActionTrampoline, this);
  app.on(ACTION_LETTER_MODE, &LibraryListActivity::letterModeActionTrampoline, this);
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
  app.clearTapFlash();
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
                             // The tab band remains focusable when there is no
                             // row. ScreenLeft then follows the OPDS header
                             // action pattern and reopens Search.
                             nav.selected = 0;
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

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  applySortOrder(orderForTab(next));
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  applySortOrder(orderForTab(index));
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
}

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

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

void LibraryListActivity::toggleLetterNameMode() {
  if (sortOrder != library::SortOrder::AuthorAsc) return;
  jumpByGivenName = !jumpByGivenName;
  // The letters present as first names are not those present as surnames.
  computeLettersPresent();
  requestUpdate();
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
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
  if (!self->letterGrid || self->sortOrder != library::SortOrder::AuthorAsc) return;
  // Tapping the already-active choice states nothing new.
  const int active = self->jumpByGivenName ? 0 : 1;
  if (event.value == active) return;
  self->toggleLetterNameMode();
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
    // Up from the top row in Author order.
    if (letterCursor < 0) {
      if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) ||
          mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) {
        toggleLetterNameMode();
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
        if (sortOrder == library::SortOrder::AuthorAsc) {
          letterCursor = -1;
          requestUpdate();
        }
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
    if (tabsFocused()) {
      openLetterGrid();
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
      stepTab(1);
    } else {
      nextPage();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft) &&
      (searchShortcutActive() || tabsFocused() || count > 0)) {
    if (searchShortcutActive()) {
      openSearch();
    } else if (tabsFocused()) {
      stepTab(-1);
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
        requestUpdate();
      }
    } else if (nav.selected - 1 > nav.top) {
      nav.selected--;
      requestUpdate();
    } else if (nav.top > 0) {
      previousPage(/*selectLast=*/true);
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

bool LibraryListActivity::searchShortcutActive() const {
  if (letterGrid || degraded) return false;
  if (ringPos() == 1) return true;
  return tabsFocused() && !query.empty() && rowCount() == 0;
}

// Touch routing while the grid consumes the loop pass: the base's routing
// never runs there, so the app's hit rects (letters, mode line) are routed
// here instead.
void LibraryListActivity::routeModalTouch() {
  const auto route = UiAppHost::routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
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
  // Capture this after syncTabListViewport(), which may clamp nav.top.
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
  if (selectLastOnNextBuild) {
    selectLastOnNextBuild = false;
    if (nav.drawnRows > 0) {
      nav.selected = nav.top + nav.drawnRows;
      nav.rebuildNeeded = true;
    }
  }
}

// One button per present letter. Author order also exposes given-name/surname
// mode; title direction is selected by its ordinary A-Z or Z-A tab.
void LibraryListActivity::buildLetterGrid(UiScreen& screen) {
  const fui::Rect body = screen.body();
  auto& target = screen.target();
  const int cell = (body.width - 2 * SIDE_PADDING) / LETTER_COLS;
  const int rows = (LETTER_COUNT + LETTER_COLS - 1) / LETTER_COLS;
  const bool hasNameMode = sortOrder == library::SortOrder::AuthorAsc;
  const int cellH = body.height / (rows + (hasNameMode ? 1 : 0));

  if (hasNameMode) {
    const char* labels[2] = {tr(STR_LIBRARY_JUMP_GIVEN), tr(STR_LIBRARY_JUMP_SURNAME)};
    const int active = jumpByGivenName ? 0 : 1;
    fui::TextStyle modeText = screen.theme().smallText;
    modeText.align = fui::TextAlign::Center;
    const int16_t modeH = target.lineHeight(modeText.font);
    constexpr int16_t MODE_GAP = 20;
    int16_t labelW[2];
    for (int i = 0; i < 2; i++) labelW[i] = target.measureText(modeText.font, labels[i], modeText).width;
    int16_t mx = static_cast<int16_t>(body.x + (body.width - (labelW[0] + labelW[1] + MODE_GAP)) / 2);
    const int16_t modeY = static_cast<int16_t>(body.y + 2);

    for (int i = 0; i < 2; i++) {
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
  }

  const int16_t top = static_cast<int16_t>(body.y + (hasNameMode ? cellH / 2 : 3));
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

void LibraryListActivity::buildHeader(UiScreen& screen) {
  fui::HeaderProps header;
  header.title = headerTitle();
  header.borderEdges = fui::EdgeBottom;
  if (!letterGrid && !degraded) {
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    const int titleFontId = uiScaleSpec().titleFontId;
    header.actionOffsetY =
        static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  }
  screen.header(header);
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it. The header itself is in the FUI layout so Search is a real action.
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});
  buildHeader(screen);

  if (letterGrid) {
    buildLetterGrid(screen);
    return;
  }

  if (!degraded) buildTabBar(screen);
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

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  // The front pair carries Left and Right here, not previous and next: it pages
  // the list, switches tabs and steps letters, none of which is one row at a
  // time. mapDirectionalLabels puts each label on whichever button actually
  // carries that screen direction.
  const char* leftLabel = letterGrid || tabsFocused() ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV);
  if (searchShortcutActive()) leftLabel = tr(STR_SEARCH);
  const char* rightLabel = letterGrid || tabsFocused() ? tr(STR_DIR_RIGHT) : tr(STR_LIBRARY_PAGE_NEXT);
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), leftLabel, rightLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void LibraryListActivity::nextPage() {
  const int count = rowCount();
  auto& nav = activeNav();
  const int next = nav.top + std::max(1, nav.pageRows());
  if (next >= count) return;
  selectLastOnNextBuild = false;
  nav.top = next;
  nav.selected = next + 1;
  requestUpdate();
}

void LibraryListActivity::previousPage(const bool selectLast) {
  auto& nav = activeNav();
  selectLastOnNextBuild = false;
  if (nav.top <= 0) return;
  nav.top = std::max(0, nav.top - std::max(1, nav.pageRows()));
  nav.selected = nav.top + 1;
  selectLastOnNextBuild = selectLast;
  requestUpdate();
}
