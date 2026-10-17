#pragma once

#include <FileIndex.h>
#include <LibraryIndexReader.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "CrossPointSettings.h"

// The contents of one directory as the file browser sees them: enumerated, filtered to the
// types the browser can open, and ordered.
//
// Two interchangeable backends behind one interface. A small folder keeps its names and
// metadata in RAM and is sorted here; a folder of INDEX_THRESHOLD entries or more builds an
// SD-backed FileIndex instead, so RAM stays bounded whatever the card holds. Should the index
// fail to build (an SD write error), the folder is listed in RAM as far as the heap has room
// for, and no further. Both present the same entry-name form -- a trailing '/' marks a
// directory -- so nothing above this class has to know which one is live.
//
// Deliberately free of rendering, input and activity transitions: it answers "what is in this
// directory, in what order", and leaves what a row MEANS to the activity. That is also why the
// path is set rather than walked -- computing the parent or a child path is navigation, and
// navigation stays with the screen.
class BuildArena;
namespace LibraryOrder {
struct BookKey;
}

class FileBrowserModel {
 public:
  // Books = the books the reader can open, and nothing else: images and the sidecars beside a
  // book (its cover, its .opf) are left out, which is what keeps an OPDS download to one row;
  // AllFiles = every file on the card, for housekeeping;
  // PickFirmware = .bin only;
  // PickFolder = directories only, for choosing a destination to move a file into.
  // Recents lists the books lately opened, newest first, from wherever they are on the card --
  // the Recent Books screen. Its rows are paths, as a card-wide search's are.
  // Added lists the books added to the card most recently (the book index's New); Authors lists the
  // book index's authors and, once one is opened, that author's books. Both read the index that
  // LibraryBuilder keeps.
  enum class Mode { Books, AllFiles, PickFirmware, PickFolder, Recents, Added, Authors };

  // Whether the reader can open this file: a book, or an image for the viewer. Listing is the
  // mode's business; this is what selecting a row can do with it.
  [[nodiscard]] static bool isOpenable(std::string_view filename);

  // Books in `dirPath` and every folder below it, by the same rules Browse Files lists them: the
  // reader's book types, hidden entries only when they are shown. A folder card's number.
  //
  // Walks the tree, so it is for the loop task, not a frame being drawn. Stops at
  // MAX_COUNTED_BOOKS (the card then says "999+"), and gives up -- returning -1 -- when the reader
  // presses a button (CooperativeAbort), so a deep tree never holds up the screen; the caller asks
  // again later.
  //
  // Every folder it finishes is remembered for the session (FolderCountMemo, 64 of them), and
  // forgotten when the card changes (HalStorage::contentGeneration) or hidden entries are shown
  // or hidden: a folder seen again is not walked again, and a count interrupted by a press picks
  // up past the folders it finished.
  static constexpr int MAX_COUNTED_BOOKS = 999;
  [[nodiscard]] static int countBooksBelow(const std::string& dirPath);
  // The remembered count for `dirPath`, without walking anything; -1 when there is none.
  [[nodiscard]] static int knownBooksBelow(const std::string& dirPath);

  explicit FileBrowserModel(const Mode mode = Mode::Books) : mode(mode) {}

  [[nodiscard]] Mode getMode() const { return mode; }
  // Switches what is listed -- the Library's tabs -- letting go of everything the old mode held. The
  // folder path is kept; load() reads the new rows.
  void setMode(const Mode newMode) {
    clear();
    mode = newMode;
  }

  // The directory currently enumerated. setPath() only records it; call load() to re-read.
  [[nodiscard]] const std::string& path() const { return basepath; }
  // Changing directory ends any search: a filter is about the folder it was run in, and
  // carrying it into the next one silently hides most of what is there.
  void setPath(std::string newPath) {
    basepath = newPath.empty() ? "/" : std::move(newPath);
    filterQuery.clear();
    matches.clear();
    clearDeepSearch();
  }

  // Re-read the directory: filter, then either build the SD index or sort in RAM. In Recents,
  // re-read the recent-books list instead.
  void load();
  // Release both backends. Called on the way out so a browser sitting on the activity stack
  // is not holding a folder's worth of names, or an open index file.
  void clear();

  // Rows presented, in display order.
  [[nodiscard]] size_t entryCount() const;
  // The row's name in the canonical form: a trailing '/' marks a directory. Out-of-range or an
  // index read failure gives "". Non-const: the SD-index backend streams from the open file.
  std::string entryName(size_t displayIndex);
  // Display index of `name` (canonical form), or entryCount() when it is not present.
  size_t findEntry(const std::string& name);

  // Narrow the rows to those whose name contains `query`, case-insensitively. "" clears it.
  //
  // A filter belongs here rather than in the screen because this class already answers "what is
  // in this directory, in what order", and because doing it here means both backends get it: the
  // match list is built through entryName(), so the SD-indexed folders that most need searching
  // are exactly the ones it works on.
  //
  // Applied once, on demand, not per keystroke. On the indexed backend building the list reads
  // every name from the index file, which is fine as the cost of pressing Search and much too
  // much to pay on each letter typed.
  void setFilter(std::string query);
  [[nodiscard]] const std::string& filter() const { return filterQuery; }
  [[nodiscard]] bool isFiltered() const { return !filterQuery.empty(); }
  // Rows the folder holds regardless of the filter. For telling someone their search matched
  // nothing in a folder that is not itself empty.
  [[nodiscard]] size_t unfilteredEntryCount() const;

  // Search every folder under the current path, not just this one.
  //
  // Walks the tree once, on demand, and keeps the paths that match. No index: FAT gives no
  // change notification, so an index would have to re-walk the tree to know whether it was
  // still true -- which is the walk it was meant to save. Walking when asked is always correct
  // and costs nothing when nobody asks.
  //
  // Results are paths relative to the search root, so two books of the same name in different
  // folders can be told apart. Capped: a reader wants to find one book, not enumerate the card.
  void searchEverywhere(const std::string& query);
  [[nodiscard]] bool isDeepSearch() const { return deepSearch; }
  // The rows are paths from all over the card -- a card-wide search, or Recents -- rather than
  // one folder's entries.
  [[nodiscard]] bool listsPaths() const {
    return deepSearch || mode == Mode::Recents || mode == Mode::Added || (mode == Mode::Authors && openAuthorRow >= 0);
  }

  // A readable book's filename, by the rule Browse Files lists books with; for the library builder.
  static bool isBookName(const char* name);

  // Authors mode, showing the author list rather than one author's books.
  [[nodiscard]] bool atAuthorList() const { return mode == Mode::Authors && openAuthorRow < 0; }
  // Shows the books of the author at `row` of the author list, by series, series index and title.
  // False when there is no such row.
  bool openAuthor(size_t row);
  // Back from an author's books to the author list; returns the row of the author that was open.
  size_t closeAuthor();
  // Opens the author whose hash is `hash`, after load(): how a return from a book, or a tab switched
  // back to, finds the author it left. False when the index has no such author.
  bool openAuthorByHash(uint32_t hash);
  // The open author's name as its row shows it; "" at the author list.
  std::string openAuthorName();
  // The open author's hash, for openAuthorByHash(); meaningful only while !atAuthorList().
  [[nodiscard]] uint32_t openAuthorKey() const { return openAuthorHash; }
  // A row of the author list: its books, and its author hash (library::AUTHOR_UNKNOWN and
  // AUTHOR_PENDING gather books with no author and books not indexed yet).
  bool authorAt(size_t row, uint16_t& books, uint32_t& hash);
  // Where an author's books are put in order: the screen's lent framebuffer, if it holds it at that
  // moment (nullptr when not, and the books keep the index's order). Asked on the loop task only.
  using ScratchSource = BuildArena* (*)(void* user);
  void setScratchSource(const ScratchSource source, void* user) {
    scratchSource = source;
    scratchUser = user;
  }
  // The book index New and Authors read. Let go before the builder replaces it; load() reopens it.
  void releaseIndex();
  [[nodiscard]] const LibraryIndexReader& index() const { return bookIndex; }
  // True when the walk stopped at MAX_DEEP_RESULTS with more still out there.
  [[nodiscard]] bool deepResultsTruncated() const { return deepTruncated; }
  // Absolute path for a row, whichever mode is live. The caller no longer composes it.
  [[nodiscard]] std::string entryFullPath(size_t displayIndex);
  // The row's file size in bytes, or 0 when not known: a directory, a card-wide search result
  // (the walk keeps paths only), or an index read failure.
  [[nodiscard]] uint32_t entrySize(size_t displayIndex);
  // Ends a card-wide search and returns the browser to the folder it was started from.
  void clearSearch() { clearDeepSearch(); }
  // Folder holding a result row, relative to the search root ("" when it sat at the root).
  [[nodiscard]] std::string resultFolder(size_t displayIndex);

  [[nodiscard]] CrossPointSettings::FILE_SORT_MODE getSortMode() const { return sortMode; }
  [[nodiscard]] CrossPointSettings::FILE_SORT_DIRECTION getSortDirection() const { return sortDirection; }
  void setSort(const CrossPointSettings::FILE_SORT_MODE mode, const CrossPointSettings::FILE_SORT_DIRECTION direction) {
    sortMode = mode;
    sortDirection = direction;
  }
  // Re-order the rows already in RAM without re-reading the directory. Does nothing while the
  // SD index is live: that one bakes its order in at build time, so a sort change there needs
  // load(). usesIndex() is how the caller tells the two apart.
  void resort();
  [[nodiscard]] bool usesIndex() const { return fileIndex != nullptr; }

 private:
  // At or above this many entries, the folder moves to the SD-backed index. Reading a folder
  // keeps no more names than this: past it the entries are only counted, and the index lists them.
  static constexpr size_t INDEX_THRESHOLD = 64;
  // Ceiling on a single search's match list, so a one-letter query in a huge folder cannot
  // turn into an unbounded allocation. Four bytes each, so this is 4 KB at worst.
  static constexpr size_t MAX_MATCHES = 1024;

  // Backend row indices that match the filter, in display order. Empty and unused when no
  // filter is set, so an unfiltered browser costs nothing.
  std::string filterQuery;
  std::vector<uint32_t> matches;

  // Card-wide search results, or the Recents list: paths relative to deepRoot ("/" for Recents).
  // Held only while one is on screen; cleared with everything else in clear() and on any
  // directory change.
  static constexpr size_t MAX_DEEP_RESULTS = 64;
  bool deepSearch = false;
  bool deepTruncated = false;
  std::string deepRoot;
  std::vector<std::string> deepResults;
  void rebuildMatches();
  void clearDeepSearch() {
    deepSearch = false;
    deepTruncated = false;
    deepResults.clear();
    deepResults.shrink_to_fit();
    deepRoot.clear();
  }
  // entryName() without the filter indirection: what the live backend holds at that row.
  std::string backendEntryName(size_t backendIndex);

  // What the browser lists, in the mode it was opened in. Both the in-RAM enumeration and the
  // index build/staleness scan go through these, so the two backends cannot disagree about
  // what the folder contains.
  //
  // One function per mode rather than one taking a Mode, because FileIndex::AcceptFn is a bare
  // function pointer with no user data: indexFilter() hands over the one that matches.
  static bool acceptForBooks(const char* name, bool isDir);
  static bool acceptForAllFiles(const char* name, bool isDir);
  static bool acceptForFirmware(const char* name, bool isDir);
  static bool acceptForFolders(const char* name, bool isDir);
  [[nodiscard]] bool acceptEntry(const char* name, bool isDir) const;
  [[nodiscard]] FileIndex::AcceptFn indexFilter() const;

  // Reads the directory into the in-RAM backend: keeps the first `keep` entries it accepts and
  // only counts the rest, and with a `heapFloor` keeps none once free heap falls below it.
  // Returns how many entries it accepted in all.
  size_t readDirectory(size_t keep, uint32_t heapFloor);
  // How many of a folder's `entries` the in-RAM backend has room for on the heap as it is now:
  // the listing for a folder whose index could not be built.
  static size_t ramListingRoom(size_t entries);
  // Lets go of the in-RAM backend's names and metadata, capacity included.
  void releaseRamListing();
  bool openIndex();
  void loadRecents();
  bool indexEntryAt(size_t displayIndex, FileIndex::Entry& out);

  void loadAdded();
  void loadAuthors();
  void orderAuthorBooks();
  static bool bookKey(void* self, uint16_t record, LibraryOrder::BookKey& key);
  ScratchSource scratchSource = nullptr;
  void* scratchUser = nullptr;
  std::string authorRowName(size_t row);
  std::string authorBookName(size_t row);

  LibraryIndexReader bookIndex;
  int openAuthorRow = -1;
  uint32_t openAuthorHash = 0;        // to find the open author again after the index is rebuilt
  std::vector<uint16_t> authorBooks;  // the open author's records, in display order

  Mode mode = Mode::Books;
  std::string basepath = "/";

  // In-RAM backend: names plus the metadata the date/size sorts need, index-aligned.
  std::vector<std::string> files;
  std::vector<uint32_t> fileSizes;      // cached file sizes (0 for directories)
  std::vector<uint32_t> fileDateTimes;  // cached FAT date/time pairs

  // SD backend: null for small folders, active for INDEX_THRESHOLD+ entries.
  std::unique_ptr<FileIndex> fileIndex;

  // Per-session, not persisted. Visibility toggles (showHiddenFiles / showFileExtensions)
  // live in SETTINGS.
  CrossPointSettings::FILE_SORT_MODE sortMode = CrossPointSettings::SORT_BY_NAME;
  CrossPointSettings::FILE_SORT_DIRECTION sortDirection = CrossPointSettings::SORT_ASCENDING;
};
