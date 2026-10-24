#include "LibraryListActivity.h"

#include <Epub.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/DictionaryDefinitionActivity.h"  // paged description viewer
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/CoverCanvas.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
constexpr unsigned long LONG_PRESS_MS = 1000;
// Cover rows: air above and below the cover, and between cover and text.
constexpr int COVER_ROW_PADDING = 4;
constexpr int COVER_TEXT_GAP = 12;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int TAB_SLOTS = AUTHOR_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_TAB_RECENT);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Three short tab labels: a full-slot pill would stretch across a third of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();
  if (SETTINGS.libraryShowCovers) covers.begin();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  covers.end();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
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

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

bool LibraryListActivity::bookPathFor(const int entry, std::string& path) {
  path.clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    path = books[static_cast<size_t>(entry)].path;
    return true;
  }
  if (!index.isOpen()) return false;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return false;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return false;
  }
  return true;
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (bookPathFor(selectedEntry(), path)) openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press opens the book options menu on every tab. On collapsed
// groups it expands the group, like a tap.
void LibraryListActivity::onRowLongPress(const int index) {
  if (groupsCollapsed) {
    activateIndex(index);
  } else {
    showBookOptions(index);
  }
}

// Book-row long-press menu (button hold and touch long-press). On the Recent
// tab the first rows may come from RecentBooksStore; only those can be
// removed from recents.
void LibraryListActivity::showBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve row %d", entry);
      return;
    }
  }

  enum class RowAction : uint8_t { Open, Description, RemoveRecent, ShowGroups, Delete, Rebuild };
  static constexpr int MAX_ACTIONS = 6;
  std::array<RowAction, MAX_ACTIONS> actions{};
  const char* labels[MAX_ACTIONS] = {};
  int actionCount = 0;
  const auto add = [&](const RowAction action, const char* label) {
    actions[actionCount] = action;
    labels[actionCount] = label;
    actionCount++;
  };
  add(RowAction::Open, tr(STR_OPEN));
  if (FsHelpers::hasEpubExtension(path)) add(RowAction::Description, tr(STR_BOOK_DESCRIPTION));
  if (isStoreRow) add(RowAction::RemoveRecent, tr(STR_REMOVE_FROM_RECENTS));
  if (query.empty() && groupable()) add(RowAction::ShowGroups, tr(STR_LIBRARY_SHOW_GROUPS));
  add(RowAction::Delete, tr(STR_DELETE));
  add(RowAction::Rebuild, tr(STR_LIBRARY_REBUILD));

  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), labels, actionCount, 0,
                   [this, path, title, entry, actions, actionCount](const int choice) {
                     swallowHeldReleases();
                     if (choice < 0 || choice >= actionCount) return;
                     switch (actions[static_cast<size_t>(choice)]) {
                       case RowAction::Open:
                         openBookByPath(path);
                         break;
                       case RowAction::Description:
                         showBookDescription(path, title);
                         break;
                       case RowAction::RemoveRecent:
                         promptRemoveRecentBook(path, title);
                         break;
                       case RowAction::ShowGroups:
                         collapseGroups(entry);
                         break;
                       case RowAction::Delete:
                         promptDeleteBookByPath(path, title);
                         break;
                       case RowAction::Rebuild:
                         promptRebuildIndex();
                         break;
                     }
                   });
  requestUpdate();
}

void LibraryListActivity::showBookDescription(const std::string& path, const std::string& title) {
  // Calibre comments are usually 1-4 KB of HTML; the cap bounds the heap the
  // string and its laid-out pages take while the viewer is open.
  static constexpr size_t MAX_DESCRIPTION_BYTES = 8 * 1024;
  std::string text;
  const bool reopenIndex = index.isOpen();
  {
    // The book wants the card to itself, and the render task must not read
    // the index while it is closed.
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    index.close();
    Epub epub(path, "/.crosspoint");
    if (!epub.loadDescription(text, MAX_DESCRIPTION_BYTES)) LOG_ERR("LIB", "cannot read description: %s", path.c_str());
  }
  const bool empty =
      std::all_of(text.begin(), text.end(), [](const char c) { return std::isspace(static_cast<unsigned char>(c)); });
  // Calibre comments are HTML; a bare "<" (as in "ages 8<12") is not a tag.
  bool html = false;
  for (size_t at = text.find('<'); !html && at != std::string::npos; at = text.find('<', at + 1)) {
    const char next = at + 1 < text.size() ? text[at + 1] : '\0';
    html = std::isalpha(static_cast<unsigned char>(next)) || next == '/' || next == '!';
  }
  if (empty) text = tr(STR_NO_BOOK_DESCRIPTION);

  auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, title, std::move(text), html);
  if (!viewer) {
    LOG_ERR("LIB", "OOM: description viewer");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }
  startActivityForResult(std::move(viewer), [this, reopenIndex](const ActivityResult&) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
  });
}

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  prepareAllCovers();
  requestUpdate(true);
}

// With covers on, Refresh library also makes every missing list thumb, newest
// books first, so paging never stops for the loading popup. Any button, a
// screen tap or the Home key stops it (the X4 Pro has no Back button); the rest
// are then made page by page as before. Runs under the caller's RenderLock,
// with the index freshly open.
void LibraryListActivity::prepareAllCovers() {
  if (!covers.ready() || !index.isOpen() || degraded) return;
  const uint16_t total = index.bookCount();
  if (total == 0) return;
  const Rect popup = GUI.drawPopup(renderer, tr(STR_LIBRARY_PREPARING_COVERS));
  GUI.fillPopupProgress(renderer, popup, 0);
  int shownPercent = 0;
  int generated = 0;
  std::string path;
  library::ClixRecord record{};
  // A button still held from choosing Refresh must not stop the pass on its
  // release, so only a press that starts during the pass counts.
  bool pressedDuringPass = false;
  for (uint16_t row = 0; row < total; ++row) {
    // Not deferred: a Home tap here only stops the pass. Buttons and taps stop
    // it on release, so the list never sees half of the gesture (a lift that
    // opens the book under the finger, a release that moves the selection).
    mappedInput.update();
    pressedDuringPass |= mappedInput.wasAnyPressed();
    int tapX = 0;
    int tapY = 0;
    if ((pressedDuringPass && mappedInput.wasAnyReleased()) || mappedInput.wasScreenTapped(tapX, tapY) ||
        mappedInput.homeButtonAction() != HomeButtonAction::Ignore) {
      LOG_INF("LIB", "cover preparation stopped at %u/%u", static_cast<unsigned>(row), static_cast<unsigned>(total));
      break;
    }
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentDesc, row);
    uint64_t key = 0;
    bool made = false;
    // The path costs a folder walk, so the thumb check comes first only where
    // it is cheap: the extension is on the name.
    if (ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readName(record, path) &&
        (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path)) && index.readPathHash(record, key) &&
        index.readPath(record, path) && LibraryCoverCache::thumbMissing(path)) {
      covers.generateFor(key, path);
      made = true;
      ++generated;
    }
    // Each progress update is a display refresh: redraw after a generated
    // thumb or every 10%, not for every book that already has one.
    const int percent = (row + 1) * 100 / total;
    if (made || percent >= shownPercent + 10) {
      GUI.fillPopupProgress(renderer, popup, percent);
      shownPercent = percent;
    }
    // Lets the idle task run through a long pass over an already covered card.
    delay(1);
  }
  LOG_INF("LIB", "prepared %d cover thumbs", generated);
  // Nothing still held from the pass may act on the list.
  swallowHeldReleases();
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
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

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (!query.empty()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (!query.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const { return !degraded && !isRecentSort(sortOrder) && bookRowCount() > 0; }

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty()) return;

  const std::string needle = library::fold(query);
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

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName,
                                     uint64_t* pathHash) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (pathHash) *pathHash = 0;
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    if (pathHash) *pathHash = library::clixPathHash(book.path.data(), book.path.size());
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
    if (pathHash && !index.readPathHash(record, *pathHash)) *pathHash = 0;
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (groupsCollapsed) {
      activateIndex(selectedEntry());
    } else if (count > 0) {
      showBookOptions(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  // Group headings carry no icon, so they keep the compact rows.
  const bool showCovers = covers.ready() && !groupsCollapsed;
  if (showCovers) {
    // The cover canvas is square with the cover at its left edge; the negative
    // part of the gap pulls the text back over the canvas's blank side. list()
    // accepts that as long as rows carry no value/toggle slot and the list is
    // not RTL (the blank side would then face the text). The explicit row
    // height also lets the viewport estimate count cover rows.
    props.iconSize = cover_canvas::SIZE;
    props.textGap = COVER_TEXT_GAP - (cover_canvas::SIZE - cover_canvas::COVER_WIDTH);
    props.rowHeight = cover_canvas::SIZE + 2 * COVER_ROW_PADDING;
    covers.beginBuild();
  }
  syncTabListViewport(screen, props);
  // Invert-fill themes draw the selected row's icon in white on black; its
  // cover gets a flipped copy so it does not show as a negative.
  const bool coverDrawnWhite = screen.theme().listSelectionStyle == fui::SelectionStyle::InvertFill;

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Only resolved for a book whose cover is not cached yet: a path costs a
  // walk over the index's folder records.
  std::string coverPath;
  uint64_t coverKey = 0;
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile, showCovers ? &coverKey : nullptr)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its cover or file-type icon.
    if (showCovers && coverKey != 0) {
      if (covers.contains(coverKey) || !bookPathFor(entry, coverPath)) coverPath.clear();
      item.icon = covers.coverFor(coverKey, coverPath, coverDrawnWhite && entry == props.selectedIndex);
    } else if (!groupsCollapsed && !rowFile.empty()) {
      item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    }
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  fui::header(screen.frame(),
              fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding), frameRect.width,
                        static_cast<int16_t>(metrics.headerHeight)},
              header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  buildHeader(screen);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  if (bookRowCount() == 0) {
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
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // book rows: hold opens the row menu
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() / 2 - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
  // Covers the page needed but the card had no thumb for: make them now that
  // the list is up, then redraw with them in place.
  if (covers.generateMissing(renderer)) requestUpdate();
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && query.empty();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
