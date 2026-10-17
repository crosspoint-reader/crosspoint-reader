#include "FileBrowserModel.h"

#include <Arduino.h>
#include <CooperativeAbort.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <LibraryOrder.h>
#include <Logging.h>
#include <Memory.h>
#include <ProtectedPaths.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

#include "BookDetails.h"
#include "FolderCountMemo.h"
#include "FolderSearch.h"
#include "LibraryFreshness.h"
#include "RecentBooksStore.h"

namespace {

// Books leaves out a folder with no book anywhere below it; All files and the Move-to-folder picker
// still list it. The accept function the SD index takes has no room for context, so the folder a
// Books load() is listing lives here while that load runs -- on the loop task, one at a time. Empty
// when folders are not being checked.
std::string emptyFolderParent;
bool folderHasBooks(const char* name);

// The later of an entry's modified and created dates, packed as FAT (date << 16) | time: the date the
// book index records.
uint32_t latestDate(HalFile& entry) {
  uint16_t date = 0;
  uint16_t time = 0;
  uint32_t latest = 0;
  if (entry.getModifyDateTime(&date, &time)) latest = (uint32_t{date} << 16) | time;
  if (entry.getCreateDateTime(&date, &time)) latest = std::max(latest, (uint32_t{date} << 16) | time);
  return latest;
}

// Folders below the root the index's walk descends to (LibraryBuilder::MAX_DEPTH): deeper books are
// not indexed, so a listing there says nothing about the index.
constexpr int INDEXED_DEPTH = 8;

int depthOf(const std::string& path) {
  int depth = 0;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '/' && (i == 0 || path[i - 1] == '/')) ++depth;
  }
  return depth;
}

// Turns the check on for one load() of a Books folder.
struct EmptyFolderCheck {
  explicit EmptyFolderCheck(const std::string& folder) {
    emptyFolderParent = folder;
    if (emptyFolderParent.empty() || emptyFolderParent.back() != '/') emptyFolderParent += '/';
  }
  ~EmptyFolderCheck() { emptyFolderParent.clear(); }
  EmptyFolderCheck(const EmptyFolderCheck&) = delete;
  EmptyFolderCheck& operator=(const EmptyFolderCheck&) = delete;
};

// The part of the filter that does not depend on what the browser is picking: a hidden entry and
// the FAT volume-information folder are never listed, whatever the mode.
bool isListableName(const char* name) {
  if (!SETTINGS.showHiddenFiles && name[0] == '.') return false;
  return strcmp(name, "System Volume Information") != 0;
}

// What the book listings add: the firmware's own folder and the system folders hold no books, so
// they stay out even with hidden files shown, as they do in the library index. All files still
// lists .crosspoint then.
bool isBookListableName(const char* name) { return isListableName(name) && !ProtectedPaths::holdsNoBooks(name); }

// The books the reader can open. Images are not books: the viewer opens them, but in the book
// browser they are mostly covers saved beside the book they belong to, listed a second time.
bool isReadableBook(const std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename);
}

bool isViewableImage(const std::string_view filename) {
  return FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
         FsHelpers::hasPngExtension(filename);
}

std::string fileExtension(const std::string& name) {
  const char* dot = strrchr(name.c_str(), '.');
  if (!dot || dot == name.c_str() || name.back() == '/') {
    return "";  // directory, no extension, or dot-file
  }
  return std::string(dot + 1);
}

// The in-RAM listing of a folder whose SD index could not be built. Free heap it leaves to the
// rest of the screen: a page of rows, a cover thumbnail, the render task. The figure is the one
// crosspoint-reader PR #2385 settled on for the same listing, from the X3's observed low point.
constexpr uint32_t RAM_LISTING_HEAP_FLOOR = 24 * 1024;
// What one kept entry costs the heap: its std::string, the block its name takes (book filenames
// run 30 to 60 bytes; with the allocator's header and the heap-poisoning canaries, about 64), its
// size and date, and its row in resort()'s index.
constexpr size_t RAM_LISTING_ENTRY_BYTES = sizeof(std::string) + 64 + 2 * sizeof(uint32_t) + sizeof(size_t);
// The four per-entry arrays -- names, sizes, dates, the sort's index -- are a block each, and are
// counted as if all of them came out of the largest free one, less some slack for the allocator.
constexpr size_t RAM_LISTING_ARRAY_BYTES = sizeof(std::string) + 2 * sizeof(uint32_t) + sizeof(size_t);
constexpr uint32_t RAM_LISTING_BLOCK_SLACK = 1024;

}  // namespace

void FileBrowserModel::load() {
  // What the last load listed goes first: its names, and an index open on the old folder.
  releaseRamListing();
  fileIndex = nullptr;
  if (mode == Mode::Recents) {
    loadRecents();
    return;
  }
  if (mode == Mode::Added) {
    loadAdded();
    return;
  }
  if (mode == Mode::Authors) {
    loadAuthors();
    return;
  }

  // The enumeration below and the SD index's own scan both go through acceptForBooks.
  std::optional<EmptyFolderCheck> emptyFolders;
  if (mode == Mode::Books) emptyFolders.emplace(basepath);

  // Names are kept only while the folder may still turn out small. A large one is counted, and the
  // SD index then reads it for itself. Reading it into RAM first held every name, size and date as
  // well, for as long as the folder was open; and the name vector's doubling asked for one 24 KB
  // block at the 513th entry, which a heap with both framebuffers in it seldom has -- the device
  // aborted on the way into the folder.
  //
  // Based on crosspoint-reader PR #2385 (Adrian Ratiu, @10ne1), reworked for the SD index: there
  // the listing is always in RAM and is cut off at a heap floor; here only the fallback is.
  const size_t entries = readDirectory(INDEX_THRESHOLD, 0);
  if (entries >= INDEX_THRESHOLD) {
    releaseRamListing();
    if (openIndex()) return;
    // No index (an SD write error): the folder is listed from RAM, as far as the heap has room.
    readDirectory(ramListingRoom(entries), RAM_LISTING_HEAP_FLOOR);
    if (files.size() < entries) {
      LOG_ERR("FBR", "Low heap: listing %u of %u entries in %s", static_cast<unsigned>(files.size()),
              static_cast<unsigned>(entries), basepath.c_str());
    }
  }
  resort();
}

size_t FileBrowserModel::readDirectory(const size_t keep, const uint32_t heapFloor) {
  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }

  root.rewindDirectory();

  // One block each for what is kept: grown entry by entry, every doubling asked for twice the room
  // while the old block was still held, and left it behind as a hole.
  files.reserve(keep);
  fileSizes.reserve(keep);
  fileDateTimes.reserve(keep);
  size_t room = keep;

  // A book or folder listed here that is newer than everything the book index knows was put on the
  // card where the firmware did not see it (LibraryFreshness::checkListedEntry).
  const bool checkForUnseen = mode == Mode::Books && depthOf(basepath) <= INDEXED_DEPTH;
  size_t accepted = 0;
  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    if (!acceptEntry(name, isDir)) {
      file.close();
      continue;
    }
    if (checkForUnseen) LibraryFreshness::checkListedEntry(latestDate(file));
    ++accepted;
    // Below the floor nothing more is kept, even should the heap recover: the list stays a prefix.
    if (files.size() < room && heapFloor > 0 && ESP.getFreeHeap() < heapFloor) room = files.size();
    if (files.size() >= room) {
      file.close();
      continue;
    }

    if (isDir) {
      std::string& dirName = files.emplace_back();
      dirName.reserve(strlen(name) + 1);  // the name and its '/' in one block
      dirName.append(name).push_back('/');
      fileSizes.push_back(0);      // directories have size 0
      fileDateTimes.push_back(0);  // will use default date
    } else {
      files.emplace_back(name);
      fileSizes.push_back(static_cast<uint32_t>(file.fileSize()));
      uint16_t fdate = 0, ftime = 0;
      file.getModifyDateTime(&fdate, &ftime);
      uint32_t combined = (static_cast<uint32_t>(fdate) << 16) | ftime;
      fileDateTimes.push_back(combined);
    }
    file.close();
  }
  root.close();
  return accepted;
}

size_t FileBrowserModel::ramListingRoom(const size_t entries) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  const size_t byHeap =
      freeHeap > RAM_LISTING_HEAP_FLOOR ? (freeHeap - RAM_LISTING_HEAP_FLOOR) / RAM_LISTING_ENTRY_BYTES : 0;
  const size_t byBlock =
      largestBlock > RAM_LISTING_BLOCK_SLACK ? (largestBlock - RAM_LISTING_BLOCK_SLACK) / RAM_LISTING_ARRAY_BYTES : 0;
  return std::min({entries, byHeap, byBlock});
}

void FileBrowserModel::releaseRamListing() {
  files.clear();
  files.shrink_to_fit();
  fileSizes.clear();
  fileSizes.shrink_to_fit();
  fileDateTimes.clear();
  fileDateTimes.shrink_to_fit();
}

bool FileBrowserModel::acceptForBooks(const char* name, const bool isDir) {
  if (!isBookListableName(name)) return false;
  // Every folder is worth descending into -- but a Books folder being listed leaves out the ones with
  // no book below them. The SD index applies this to its staleness scan too, so a folder that gains
  // a book changes the index's signature and comes back.
  if (isDir) return emptyFolderParent.empty() || folderHasBooks(name);
  return isReadableBook(std::string_view{name});
}

bool FileBrowserModel::isOpenable(const std::string_view filename) {
  return isReadableBook(filename) || isViewableImage(filename);
}

namespace {

// Folder counts already worked out this session (see FolderCountMemo). One table for every
// browser, so leaving Browse Files and coming back keeps them; 512 bytes, whatever the library.
// The render task reads it (a folder card being drawn) while the loop task's walk writes it.
FolderCountMemo folderCounts;
std::mutex folderCountsMutex;

int rememberedCount(const std::string& folder, const uint32_t stamp) {
  std::lock_guard<std::mutex> guard(folderCountsMutex);
  return folderCounts.find(folder, stamp);
}

void rememberCount(const std::string& folder, const int books, const uint32_t stamp) {
  std::lock_guard<std::mutex> guard(folderCountsMutex);
  folderCounts.store(folder, books, stamp);
}

// What a count depends on besides the folder: what is on the card, and whether hidden entries
// are counted (isBookListableName).
uint32_t countStamp() { return (Storage.contentGeneration() << 1) | (SETTINGS.showHiddenFiles ? 1u : 0u); }

}  // namespace

int FileBrowserModel::knownBooksBelow(const std::string& dirPath) { return rememberedCount(dirPath, countStamp()); }

namespace {

// A folder in the one a Books load() is listing: does it hold a book anywhere below? Stops at the
// first book. What the folder counts already know is used, and every folder searched to its end
// without one is recorded there as holding none -- an exact count -- so it is not walked again until
// the card changes. A press gives up, and the folder is listed.
bool folderHasBooks(const char* name) {
  FolderSearch::Rules rules;
  rules.listable = &isBookListableName;
  rules.wanted = &FileBrowserModel::isBookName;
  rules.known = [](void*, const std::string& path) { return rememberedCount(path, countStamp()); };
  rules.foundNone = [](void*, const std::string& path) { rememberCount(path, 0, countStamp()); };
  rules.stop = [](void*) {
    HalSystem::feedWatchdog();
    return CooperativeAbort::shouldAbortLongTask();
  };
  return FolderSearch::anyBelow(emptyFolderParent + name, rules);
}

}  // namespace

// Depth first, one open directory per level -- at most MAX_DEPTH + 1 of them, however wide the
// tree -- so a folder's total is complete when its directory runs out, and is recorded then.
// A folder already in the table is added without being entered, so a count interrupted by a
// button press resumes past everything it finished, and counting a parent after a child (or the
// other way round) walks each folder once.
int FileBrowserModel::countBooksBelow(const std::string& dirPath) {
  constexpr int MAX_DEPTH = 8;  // as deep as anyone files books; a cycle-proof bound besides
  const uint32_t stamp = countStamp();
  if (const int known = rememberedCount(dirPath, stamp); known >= 0) return known;

  struct Level {
    HalFile dir;
    std::string path;
    int books = 0;
    bool complete = true;  // nothing below was cut off by MAX_DEPTH
  };
  std::vector<Level> levels;
  levels.reserve(MAX_DEPTH + 1);  // references into it stay valid while it grows
  {
    auto root = Storage.open(dirPath.c_str());
    if (!root || !root.isDirectory()) return 0;
    root.rewindDirectory();
    levels.push_back(Level{std::move(root), dirPath});
  }
  int counted = 0;  // every book found so far, for the cap
  char name[500];
  while (true) {
    if (CooperativeAbort::shouldAbortLongTask()) return -1;  // the levels close their directories
    if (counted > MAX_COUNTED_BOOKS) {
      // More than the card can say. The folder asked about is recorded as such; the ones still
      // open below it are not finished, so they are not recorded at all.
      rememberCount(dirPath, MAX_COUNTED_BOOKS + 1, stamp);
      return MAX_COUNTED_BOOKS + 1;
    }
    Level& level = levels.back();
    auto entry = level.dir.openNextFile();
    if (!entry) {
      // This folder is done. Its total is recorded when nothing in it was cut off by depth, and
      // always for the folder asked about -- that is the number on its card.
      level.dir.close();
      const int books = level.books;
      const bool complete = level.complete;
      if (complete || levels.size() == 1) rememberCount(level.path, books, stamp);
      levels.pop_back();
      if (levels.empty()) return std::min(books, MAX_COUNTED_BOOKS + 1);
      levels.back().books += books;
      levels.back().complete = levels.back().complete && complete;
      HalSystem::feedWatchdog();
      continue;
    }
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!isBookListableName(name)) continue;
    if (!isDir) {
      if (isReadableBook(std::string_view{name})) {
        ++level.books;
        ++counted;
      }
      continue;
    }
    std::string child = level.path + "/" + name;
    if (const int known = rememberedCount(child, stamp); known >= 0) {
      level.books += known;
      counted += known;
      continue;
    }
    if (levels.size() > MAX_DEPTH) {
      level.complete = false;  // deeper than we look: this total is a lower bound
      continue;
    }
    auto sub = Storage.open(child.c_str());
    if (!sub || !sub.isDirectory()) continue;
    sub.rewindDirectory();
    levels.push_back(Level{std::move(sub), std::move(child)});
  }
}

bool FileBrowserModel::acceptForAllFiles(const char* name, const bool /*isDir*/) { return isListableName(name); }

bool FileBrowserModel::acceptForFirmware(const char* name, const bool isDir) {
  if (!isListableName(name)) return false;
  if (isDir) return true;
  return FsHelpers::checkFileExtension(std::string_view{name}, ".bin");
}

bool FileBrowserModel::acceptForFolders(const char* name, const bool isDir) {
  // Only somewhere a file can be put. Listing the files too would be scenery to scroll past.
  return isListableName(name) && isDir;
}

bool FileBrowserModel::acceptEntry(const char* name, const bool isDir) const {
  switch (mode) {
    case Mode::PickFirmware:
      return acceptForFirmware(name, isDir);
    case Mode::PickFolder:
      return acceptForFolders(name, isDir);
    case Mode::AllFiles:
      return acceptForAllFiles(name, isDir);
    case Mode::Books:
    case Mode::Recents:
      break;
  }
  return acceptForBooks(name, isDir);
}

FileIndex::AcceptFn FileBrowserModel::indexFilter() const {
  switch (mode) {
    case Mode::PickFirmware:
      return &acceptForFirmware;
    case Mode::PickFolder:
      return &acceptForFolders;
    case Mode::AllFiles:
      return &acceptForAllFiles;
    case Mode::Books:
    case Mode::Recents:
      break;
  }
  return &acceptForBooks;
}

// The SD index for a folder of INDEX_THRESHOLD entries or more. False when it could not be had,
// and the folder is then listed from RAM.
bool FileBrowserModel::openIndex() {
  fileIndex = makeUniqueNoThrow<FileIndex>();
  if (!fileIndex) {
    LOG_ERR("FBR", "OOM: FileIndex for %s", basepath.c_str());
    return false;
  }
  const FileIndex::SortMode indexSortMode = static_cast<FileIndex::SortMode>(sortMode);
  if (!fileIndex->open(basepath.c_str(), indexSortMode, indexFilter())) {
    LOG_ERR("FBR", "FileIndex build failed for %s, falling back to in-RAM sort", basepath.c_str());
    fileIndex = nullptr;
    return false;
  }
  rebuildMatches();  // the rows were just renumbered
  return true;
}

size_t FileBrowserModel::unfilteredEntryCount() const { return fileIndex ? fileIndex->totalCount() : files.size(); }

size_t FileBrowserModel::entryCount() const {
  if (mode == Mode::Authors) return atAuthorList() ? bookIndex.header().authorCount : authorBooks.size();
  if (listsPaths()) return deepResults.size();
  return isFiltered() ? matches.size() : unfilteredEntryCount();
}

bool FileBrowserModel::indexEntryAt(const size_t displayIndex, FileIndex::Entry& out) {
  if (!fileIndex) return false;
  const bool desc = (sortDirection == CrossPointSettings::SORT_DESCENDING);
  return fileIndex->entryAt(displayIndex, desc, out);
}

// Returns the row's name in the canonical browser form: a trailing '/' marks a
// directory. For the in-RAM backend `files` already stores this form; for the SD
// index we reconstruct it from the Entry. Out-of-range / index-read failure → "".
std::string FileBrowserModel::entryName(const size_t displayIndex) {
  if (mode == Mode::Authors) return atAuthorList() ? authorRowName(displayIndex) : authorBookName(displayIndex);
  if (listsPaths()) {
    return displayIndex < deepResults.size() ? deepResults[displayIndex] : "";
  }
  if (isFiltered()) {
    if (displayIndex >= matches.size()) return "";
    return backendEntryName(matches[displayIndex]);
  }
  return backendEntryName(displayIndex);
}

std::string FileBrowserModel::backendEntryName(const size_t displayIndex) {
  FileIndex::Entry e;
  if (indexEntryAt(displayIndex, e)) {
    std::string name(e.name);
    if (e.isDir) name += '/';
    return name;
  }
  if (fileIndex || displayIndex >= files.size()) return "";  // index read failed, or OOR
  return files[displayIndex];
}

size_t FileBrowserModel::findEntry(const std::string& name) {
  if (mode == Mode::Authors) {
    const size_t count = entryCount();
    for (size_t i = 0; i < count; i++)
      if (entryName(i) == name) return i;
    return count;
  }
  if (listsPaths()) {
    for (size_t i = 0; i < deepResults.size(); i++)
      if (deepResults[i] == name) return i;
    return deepResults.size();
  }
  if (isFiltered()) {
    for (size_t i = 0; i < matches.size(); i++)
      if (backendEntryName(matches[i]) == name) return i;
    return matches.size();
  }
  if (fileIndex) {
    // The index stores names without the trailing '/'; strip it for the lookup.
    std::string bare = name;
    if (!bare.empty() && bare.back() == '/') bare.pop_back();
    const bool desc = (sortDirection == CrossPointSettings::SORT_DESCENDING);
    const size_t row = fileIndex->findRowByName(bare.c_str(), desc);
    return (row == SIZE_MAX) ? entryCount() : row;
  }
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return files.size();
}

void FileBrowserModel::setFilter(std::string query) {
  // Trim: a stray space from the keyboard should not be the reason nothing matches.
  while (!query.empty() && query.front() == ' ') query.erase(query.begin());
  while (!query.empty() && query.back() == ' ') query.pop_back();
  if (query == filterQuery) return;
  filterQuery = std::move(query);
  rebuildMatches();
}

void FileBrowserModel::rebuildMatches() {
  matches.clear();
  if (filterQuery.empty()) {
    matches.shrink_to_fit();  // an unfiltered browser should not keep the capacity around
    return;
  }
  // ASCII-folded substring match. Deliberately not a full Unicode fold: filenames on these cards
  // are overwhelmingly ASCII, and a UTF-8 case table costs more flash than the feature does.
  std::string needle = filterQuery;
  for (char& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

  const size_t total = unfilteredEntryCount();
  for (size_t i = 0; i < total && matches.size() < MAX_MATCHES; i++) {
    std::string name = backendEntryName(i);
    if (name.empty()) continue;
    if (name.back() == '/') name.pop_back();  // match on the name, not the marker
    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (name.find(needle) != std::string::npos) matches.push_back(static_cast<uint32_t>(i));
  }
}

std::string FileBrowserModel::entryFullPath(const size_t displayIndex) {
  const std::string name = entryName(displayIndex);
  if (name.empty()) return "";
  const std::string& base = listsPaths() ? deepRoot : basepath;
  std::string full = base;
  if (full.empty() || full.back() != '/') full += '/';
  full += name;
  if (!full.empty() && full.back() == '/') full.pop_back();  // directories carry a marker
  return full;
}

uint32_t FileBrowserModel::entrySize(const size_t displayIndex) {
  if (listsPaths()) return 0;
  size_t backendIndex = displayIndex;
  if (isFiltered()) {
    if (displayIndex >= matches.size()) return 0;
    backendIndex = matches[displayIndex];
  }
  FileIndex::Entry e;
  if (indexEntryAt(backendIndex, e)) return e.isDir ? 0 : e.size;
  if (fileIndex || backendIndex >= fileSizes.size()) return 0;
  return fileSizes[backendIndex];
}

std::string FileBrowserModel::resultFolder(const size_t displayIndex) {
  if (!listsPaths() || displayIndex >= entryCount()) return "";
  const std::string rel = entryName(displayIndex);
  const size_t slash = rel.rfind('/');
  if (slash == std::string::npos) return deepRoot;  // it sat in the search root
  std::string folder = deepRoot;
  if (folder.empty() || folder.back() != '/') folder += '/';
  folder += rel.substr(0, slash);
  return folder;
}

void FileBrowserModel::searchEverywhere(const std::string& query) {
  clearDeepSearch();
  if (query.empty()) return;

  std::string needle = query;
  for (char& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

  deepRoot = basepath;
  deepSearch = true;

  // Depth first, one open directory per level, as countBooksBelow walks: what the search holds is
  // bounded by how deep it goes, not by how wide a folder is. A queue of the folders still to visit
  // held a path for every one of them, and above a library of a thousand author folders that queue
  // alone ran the heap out. Explicit levels, not recursion: the reader's task stack is not the
  // place to find out how deep a card goes.
  constexpr int MAX_DEPTH = 8;  // as countBooksBelow and the book index look
  struct Level {
    HalFile dir;
    std::string path;
  };
  std::vector<Level> levels;
  levels.reserve(MAX_DEPTH + 1);  // references into it stay valid while it grows
  {
    auto root = Storage.open(basepath.c_str());
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      return;
    }
    root.rewindDirectory();
    levels.push_back(Level{std::move(root), basepath});
  }

  char name[500];
  while (!levels.empty() && deepResults.size() < MAX_DEEP_RESULTS) {
    Level& level = levels.back();
    auto entry = level.dir.openNextFile();
    if (!entry) {
      level.dir.close();
      levels.pop_back();
      continue;
    }
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    // "." and ".." always; anything else beginning with a dot only when the browser is
    // showing hidden files, so a search sees exactly what browsing would.
    if (name[0] == '.') {
      const bool dotdot = (name[1] == '\0') || (name[1] == '.' && name[2] == '\0');
      if (dotdot || !SETTINGS.showHiddenFiles) continue;
    }
    std::string child = level.path;
    if (child.empty() || child.back() != '/') child += '/';
    child += name;
    if (isDir) {
      if (!acceptEntry(name, true) || static_cast<int>(levels.size()) > MAX_DEPTH) continue;
      auto sub = Storage.open(child.c_str());
      if (!sub || !sub.isDirectory()) {
        if (sub) sub.close();
        continue;
      }
      sub.rewindDirectory();
      levels.push_back(Level{std::move(sub), std::move(child)});
      continue;
    }
    if (!acceptEntry(name, false)) continue;
    std::string folded = name;
    for (char& c : folded) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (folded.find(needle) == std::string::npos) continue;
    // Stored relative to the root, so the row shows which folder it came from.
    deepResults.push_back(child.substr(deepRoot.size() + (deepRoot.back() == '/' ? 0 : 1)));
  }
  // Stopped at the cap with folders still unread: there may be more.
  if (!levels.empty()) deepTruncated = true;
}

void FileBrowserModel::resort() {
  if (fileIndex || listsPaths() || mode == Mode::Authors) return;  // ordered at build time / by the index
  // Whatever happens below renumbers the rows, so the match list is rebuilt at the end.
  // Create index array to preserve metadata array alignment
  std::vector<size_t> indices(files.size());
  for (size_t i = 0; i < files.size(); ++i) indices[i] = i;

  std::sort(indices.begin(), indices.end(), [this](size_t idx_a, size_t idx_b) {
    const std::string& a = files[idx_a];
    const std::string& b = files[idx_b];
    const bool isDir_a = a.back() == '/';
    const bool isDir_b = b.back() == '/';

    // Directories always sort first
    if (isDir_a != isDir_b) return isDir_a;

    // Both are directories or both are files; apply sort mode
    const char* name_a = a.c_str();
    const char* name_b = b.c_str();

    int cmp = 0;  // -1 if a < b, 0 if equal, +1 if a > b

    switch (sortMode) {
      case CrossPointSettings::SORT_BY_NAME:
        cmp = FsHelpers::naturalCompare(name_a, name_b);
        break;

      case CrossPointSettings::SORT_BY_DATE: {
        // Use cached metadata (no file opens)
        uint32_t dt_a = (idx_a < fileDateTimes.size()) ? fileDateTimes[idx_a] : 0;
        uint32_t dt_b = (idx_b < fileDateTimes.size()) ? fileDateTimes[idx_b] : 0;
        if (dt_a < dt_b) {
          cmp = -1;
        } else if (dt_a > dt_b) {
          cmp = 1;
        } else {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      case CrossPointSettings::SORT_BY_SIZE: {
        // Use cached metadata (no file opens)
        uint32_t size_a = (idx_a < fileSizes.size()) ? fileSizes[idx_a] : 0;
        uint32_t size_b = (idx_b < fileSizes.size()) ? fileSizes[idx_b] : 0;
        if (size_a < size_b) {
          cmp = -1;
        } else if (size_a > size_b) {
          cmp = 1;
        } else {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      case CrossPointSettings::SORT_BY_TYPE: {
        std::string ext_a = fileExtension(a);
        std::string ext_b = fileExtension(b);
        // Case-insensitive extension comparison
        std::transform(ext_a.begin(), ext_a.end(), ext_a.begin(), ::tolower);
        std::transform(ext_b.begin(), ext_b.end(), ext_b.begin(), ::tolower);
        cmp = FsHelpers::naturalCompare(ext_a.c_str(), ext_b.c_str());
        if (cmp == 0) {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      default:
        cmp = 0;
    }

    // Apply sort direction
    if (sortDirection == CrossPointSettings::SORT_DESCENDING) {
      cmp = -cmp;
    }

    return cmp < 0;
  });

  // Put the rows in that order in place, one cycle of the permutation at a time (indices[row] is
  // the entry that belongs at row): each name is moved to its row, not copied into a second
  // listing, which held every name twice at once. A row done is marked by indices[row] == row.
  for (size_t start = 0; start < indices.size(); ++start) {
    if (indices[start] == start) continue;
    std::string name = std::move(files[start]);
    const uint32_t size = fileSizes[start];
    const uint32_t dateTime = fileDateTimes[start];
    size_t row = start;
    while (indices[row] != start) {
      const size_t from = indices[row];
      files[row] = std::move(files[from]);
      fileSizes[row] = fileSizes[from];
      fileDateTimes[row] = fileDateTimes[from];
      indices[row] = row;
      row = from;
    }
    files[row] = std::move(name);
    fileSizes[row] = size;
    fileDateTimes[row] = dateTime;
    indices[row] = row;
  }
  rebuildMatches();  // the rows were just renumbered
}

// The recent-books list, newest first, as paths relative to "/" -- the shape a card-wide search's
// results already have, so every row accessor serves both. Books no longer on the card are dropped
// from the list for good, as the Recent Books screen always did on the way in.
void FileBrowserModel::loadRecents() {
  clearDeepSearch();
  // One load of the list for the prune, its save and the read below; the paths are copied out, so
  // nothing here outlives the hold (RecentBooksStore::Hold).
  const RecentBooksStore::Hold recents;
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  deepRoot = "/";
  const auto& books = RECENT_BOOKS.getBooks();
  deepResults.reserve(books.size());
  for (const auto& book : books) {
    if (book.path.size() > 1 && book.path.front() == '/') deepResults.push_back(book.path.substr(1));
  }
}

void FileBrowserModel::clear() {
  releaseRamListing();
  filterQuery.clear();
  matches.clear();
  matches.shrink_to_fit();
  clearDeepSearch();
  bookIndex.close();
  openAuthorRow = -1;
  authorBooks.clear();
  authorBooks.shrink_to_fit();
  if (fileIndex) fileIndex->close();
  fileIndex = nullptr;
}

bool FileBrowserModel::isBookName(const char* name) { return isReadableBook(std::string_view{name}); }

void FileBrowserModel::releaseIndex() { bookIndex.close(); }

// The index's newest books, as paths from the root like Recents' rows. A book gone since the index
// was built is left out.
void FileBrowserModel::loadAdded() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen() && !bookIndex.open(library::INDEX_PATH)) return;
  std::string bookPath;
  library::BookRecord record{};
  for (uint16_t rank = 0; rank < bookIndex.header().newCount; ++rank) {
    uint16_t recordIndex = 0;
    if (!bookIndex.newBook(rank, recordIndex) || !bookIndex.book(recordIndex, record) ||
        !bookIndex.blobString(record.pathOff, bookPath)) {
      continue;
    }
    if (bookPath.size() > 1 && bookPath.front() == '/' && Storage.exists(bookPath.c_str()))
      deepResults.push_back(bookPath.substr(1));
  }
}

// The author list, or -- when an author was open -- that author's books again, found by hash: a
// rebuilt index may have moved its row.
void FileBrowserModel::loadAuthors() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen()) bookIndex.open(library::INDEX_PATH);
  if (openAuthorRow < 0) return;
  closeAuthor();
  openAuthorByHash(openAuthorHash);
}

bool FileBrowserModel::openAuthorByHash(const uint32_t hash) {
  if (mode != Mode::Authors) return false;
  if (openAuthorRow >= 0) closeAuthor();
  library::AuthorRecord author{};
  for (uint16_t row = 0; row < bookIndex.header().authorCount; ++row) {
    if (bookIndex.author(row, author) && author.hash == hash) return openAuthor(row);
  }
  return false;
}

// The opened author's header: the name as the books spell it ("Terry Pratchett").
std::string FileBrowserModel::openAuthorName() {
  if (mode != Mode::Authors || openAuthorRow < 0) return "";
  library::AuthorRecord author{};
  std::string name;
  if (!bookIndex.author(static_cast<uint16_t>(openAuthorRow), author)) return "";
  if (author.hash == library::AUTHOR_PENDING) return tr(STR_NOT_YET_INDEXED);
  if (author.hash == library::AUTHOR_UNKNOWN || !bookIndex.authorName(author, name) || name.empty()) {
    return tr(STR_UNKNOWN_AUTHOR);
  }
  return name;
}

bool FileBrowserModel::openAuthor(const size_t row) {
  library::AuthorRecord author{};
  if (!atAuthorList() || row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return false;
  authorBooks.clear();
  authorBooks.reserve(author.count);
  for (uint32_t slot = author.firstBook; slot < uint32_t{author.firstBook} + author.count; ++slot) {
    uint16_t record = 0;
    if (bookIndex.authorBook(slot, record)) authorBooks.push_back(record);
  }
  openAuthorRow = static_cast<int>(row);
  openAuthorHash = author.hash;
  deepRoot = "/";
  orderAuthorBooks();
  return true;
}

size_t FileBrowserModel::closeAuthor() {
  const int row = openAuthorRow;
  openAuthorRow = -1;
  authorBooks.clear();
  authorBooks.shrink_to_fit();
  return row < 0 ? 0 : static_cast<size_t>(row);
}

bool FileBrowserModel::authorAt(const size_t row, uint16_t& books, uint32_t& hash) {
  library::AuthorRecord author{};
  if (row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return false;
  books = author.count;
  hash = author.hash;
  return true;
}

// An author row, marked as a folder: opening it lists the author's books. It shows the filing name
// ("Pratchett, Terry"), which is what the list is sorted by.
std::string FileBrowserModel::authorRowName(const size_t row) {
  library::AuthorRecord author{};
  std::string name;
  std::string filing;
  if (row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return "";
  if (author.hash == library::AUTHOR_PENDING) {
    name = tr(STR_NOT_YET_INDEXED);
  } else if (author.hash == library::AUTHOR_UNKNOWN || !bookIndex.authorName(author, name, &filing) || name.empty()) {
    name = tr(STR_UNKNOWN_AUTHOR);
  } else if (!filing.empty()) {
    name = std::move(filing);
  }
  return name + '/';
}

std::string FileBrowserModel::authorBookName(const size_t row) {
  library::BookRecord record{};
  std::string bookPath;
  if (row >= authorBooks.size() || !bookIndex.book(authorBooks[row], record) ||
      !bookIndex.blobString(record.pathOff, bookPath) || bookPath.size() < 2) {
    return "";
  }
  return bookPath.substr(1);
}

// An author's books by series, then series index, then title, as the book lists show them; books in
// no series after the series. Sorted in the lent framebuffer (LibraryOrder), never on the heap. Without
// it, or past MAX_ORDERED books -- the details reads would take seconds -- the index's order stands.
void FileBrowserModel::orderAuthorBooks() {
  constexpr size_t MAX_ORDERED = 200;
  if (authorBooks.size() < 2 || authorBooks.size() > MAX_ORDERED || scratchSource == nullptr) return;
  BuildArena* scratch = scratchSource(scratchUser);
  if (scratch == nullptr) return;
  LibraryOrder::sortBySeries(authorBooks.data(), authorBooks.size(), *scratch, &FileBrowserModel::bookKey, this);
}

// What one of the open author's books sorts by: its details, and its filename when it has no title.
bool FileBrowserModel::bookKey(void* self, const uint16_t record, LibraryOrder::BookKey& key) {
  auto& model = *static_cast<FileBrowserModel*>(self);
  library::BookRecord book{};
  std::string bookPath;
  if (!model.bookIndex.book(record, book) || !model.bookIndex.blobString(book.pathOff, bookPath)) return false;
  BookDetails details;
  if (BookDetailsLookup::cached(bookPath, 0, details)) {
    key.series = std::move(details.series);
    key.seriesIndex = std::move(details.seriesIndex);
    key.title = std::move(details.title);
  }
  if (key.title.empty()) key.title = bookPath.substr(bookPath.rfind('/') + 1);
  return true;
}
