#include "LibraryBuilder.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "LibraryIndexFile.h"
#include "LibraryText.h"

namespace library {
namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/library.idx";
constexpr char NEW_PATH[] = "/.crosspoint/library.new";
constexpr char BACKUP_PATH[] = "/.crosspoint/library.bak";
constexpr char STAGE_PATH[] = "/.crosspoint/library.stage";
constexpr char CACHE_DIR[] = "/.crosspoint";

// Matches lib/FileIndex's buffer so a name this walk accepts is one the file
// browser could also show.
constexpr size_t NAME_BUF_SIZE = 512;

// One staged entry: the record as far as the filename can fill it, followed by
// the display name. Fixed stride keeps the second pass a seek rather than a scan.
constexpr size_t STAGE_NAME_BYTES = 255;
constexpr size_t STAGE_AUTHOR_BYTES = 128;
// A folder path is stored behind one length byte in the folder section.
constexpr size_t FOLDER_PATH_BYTES = 255;

// A book's series as the book itself names it. `index` leads so the whole struct
// is one contiguous run with no interior padding.
struct StagedSeries {
  uint16_t index = SERIES_INDEX_NONE;
  uint8_t len = 0;
  char name[CLIX_SERIES_NAME_BYTES] = {};
};

struct StagedEntry {
  ClixRecord record;
  char name[STAGE_NAME_BYTES];
  // Display spelling as this one filename gave it. The spelling actually shown
  // is chosen later, across every book by the same person.
  uint8_t authorLen;
  char author[STAGE_AUTHOR_BYTES];
  // The title the book gives itself, kept SEPARATE from `name`. Writing it into
  // the name slot was a defect: readPath rebuilds a book's file path from that
  // slot, so an enriched book resolved to "/Books/Germinal" and could not be
  // opened, and reconciliation hashed a dirent name on one side against a stored
  // title on the other.
  uint8_t titleLen;
  char title[STAGE_NAME_BYTES];
  // The series exactly as the book names it, before the library-wide table gives
  // it an id. Its own struct so the series pass can seek to it and read it whole
  // rather than pulling in the whole staged entry per book.
  StagedSeries series;
};
constexpr size_t STAGE_STRIDE = sizeof(StagedEntry);

// Sort array element. Holding a 12-byte key prefix rather than the whole fold
// keeps this at 14 bytes per book; ties fall back to the ordinal, so the order
// is total and a rebuild cannot shuffle equal-prefix books between runs.
struct SortKey {
  char key[12];
  uint16_t ordinal;
};
static_assert(sizeof(SortKey) == 14, "SortKey must stay small: it is the only per-book resident cost");

constexpr uint8_t MAX_AUTHOR_SPELLINGS = 16;
struct SpellingSlot {
  char text[STAGE_AUTHOR_BYTES];
  uint16_t ordinal;
  uint16_t count;
  uint8_t len;
};
static_assert(sizeof(SpellingSlot) <= 136, "spelling vote scratch grew unexpectedly");

bool sortKeyLess(const SortKey& a, const SortKey& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  return a.ordinal < b.ordinal;
}

// How much of a folded series name places a group on the shelf. Ordering only:
// what makes two series the SAME series is the digest below, not these bytes.
// Unlike authorKey's 12, this key is read by the shelf rather than by a vote, so
// a collision here is a wrong catalogue rather than a missed merge.
constexpr size_t SERIES_KEY_NAME_BYTES = 16;
// Digest of the whole folded name, which is what separates series the 16 bytes
// above cannot tell apart — "A Song of Ice and Fire" from "A Song of Ice and
// Fire: Graphic Novels", a series from its own anthology. Storing all 61 name
// bytes instead would be the exact answer, but this array is per-book resident
// during a rebuild and 61 bytes a book does not fit the budget.
constexpr size_t SERIES_KEY_HASH_BYTES = 4;
// Name and digest together decide identity; the position after them decides
// order within the group.
constexpr size_t SERIES_KEY_ID_BYTES = SERIES_KEY_NAME_BYTES + SERIES_KEY_HASH_BYTES;

// Sort key for series order: the folded name, a digest of it, then the position.
//
// One memcmp orders the whole shelf — series A-Z, and inside each the books by
// position — because the position sits in the low bytes big-endian. That means
// no second pass groups the books; the groups fall out as runs of equal identity
// bytes. SERIES_INDEX_NONE being 0xFFFF is what puts a book with no stated
// position after its numbered siblings rather than before them.
//
// The digest has to sit BEFORE the position, not after: were it last, two series
// sharing the 16 name bytes would interleave by position and neither would form
// a contiguous run for the grouping pass to find.
struct SeriesSortKey {
  char key[SERIES_KEY_ID_BYTES + 2];
  uint16_t ordinal;
};
static_assert(sizeof(SeriesSortKey) == 24, "SeriesSortKey is per-book resident cost during a rebuild");

bool seriesKeyLess(const SeriesSortKey& a, const SeriesSortKey& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  return a.ordinal < b.ordinal;
}

bool sameSeriesGroup(const SeriesSortKey& a, const SeriesSortKey& b) {
  return memcmp(a.key, b.key, SERIES_KEY_ID_BYTES) == 0;
}

// Let FreeRTOS run the idle task during every long phase, including builds
// without a UI callback and the sort/emit work after the directory walk. The
// counter keeps the delay out of tight per-byte operations while bounding CPU
// work between yields.
void serviceBuilder(uint32_t& workUnits) {
  if ((++workUnits & 0x1Fu) == 0) delay(1);
}

bool recoverInterruptedInstall() {
  if (!Storage.exists(BACKUP_PATH)) return true;
  if (!Storage.exists(INDEX_PATH)) {
    if (Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      LOG_INF("LIBIDX", "restored previous index after interrupted install");
      return true;
    }
    LOG_ERR("LIBIDX", "cannot restore %s; rebuild deferred", BACKUP_PATH);
    return false;
  }

  // Both names exist when power was lost after the new index became live but
  // before backup cleanup. Validate them one at a time (SdFat has one reader)
  // before deciding which copy is stale.
  LibraryIndexFile candidate;
  if (candidate.open(INDEX_PATH)) {
    candidate.close();
    if (Storage.remove(BACKUP_PATH)) return true;
    LOG_ERR("LIBIDX", "cannot remove stale backup; rebuild deferred");
    return false;
  }
  candidate.close();

  if (candidate.open(BACKUP_PATH)) {
    candidate.close();
    if (!Storage.remove(INDEX_PATH) || !Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      LOG_ERR("LIBIDX", "validated backup could not replace an invalid live index");
      return false;
    }
    LOG_INF("LIBIDX", "restored previous index after interrupted install");
    return true;
  }
  candidate.close();

  // Neither file validates. The live path will be preserved until a complete
  // new index is ready; the unusable backup only blocks transactional install.
  if (!Storage.remove(BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "invalid stale backup cannot be removed; rebuild deferred");
    return false;
  }
  return true;
}

bool installNewIndex() {
  const bool hadPrevious = Storage.exists(INDEX_PATH);

  // A backup beside a live index is left by a successful install interrupted
  // before cleanup. It is stale now; remove it before reserving that name for
  // the current previous index.
  if (Storage.exists(BACKUP_PATH) && !Storage.remove(BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "cannot remove stale backup; keeping the live index");
    Storage.remove(NEW_PATH);
    return false;
  }

  if (hadPrevious && !Storage.rename(INDEX_PATH, BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "cannot stage previous index for replacement");
    Storage.remove(NEW_PATH);
    return false;
  }

  if (!Storage.rename(NEW_PATH, INDEX_PATH)) {
    LOG_ERR("LIBIDX", "rename %s -> %s failed", NEW_PATH, INDEX_PATH);
    if (hadPrevious && !Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      // recoverInterruptedInstall() retries this on the next rebuild. Do not
      // remove the backup: it is the only complete index left.
      LOG_ERR("LIBIDX", "previous index rollback failed; backup retained at %s", BACKUP_PATH);
    }
    Storage.remove(NEW_PATH);
    return false;
  }

  if (hadPrevious && !Storage.remove(BACKUP_PATH)) {
    // The new live index is already complete. A stale backup is harmless and is
    // removed before the next replacement attempt.
    LOG_ERR("LIBIDX", "new index installed but stale backup cleanup failed");
  }
  return true;
}

bool isBookName(const std::string& name) {
  return FsHelpers::checkFileExtension(name, ".epub") || FsHelpers::checkFileExtension(name, ".txt") ||
         FsHelpers::checkFileExtension(name, ".md") || FsHelpers::checkFileExtension(name, ".xtc");
}

// macOS AppleDouble sidecars and hidden entries. The file browser already hides
// these (FileBrowserActivity isMacOSMetadataEntry); the shelf must agree, or a
// card written on a Mac shows every book twice.
bool isHiddenOrSidecar(const char* name) { return name[0] == '.'; }

std::string stemOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

// One book as the PREVIOUS index knew it, kept only long enough to recognise the
// same book in the new walk.
//
// The name is held as a 32-bit hash rather than as text: 512 real names are
// ~40 KB, the hashes are 6 KB, and the size check beside it makes a hash
// collision harmless. Identity is (name, size) — a name whose size changed is a
// different file, and gets re-read.
struct PriorEntry {
  uint32_t nameHash;
  uint32_t size;
  uint16_t firstSeen;
  bool matched;
};

uint32_t fnv1a32(const char* data, const size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

// Sentinel written into a staged record whose book matched nothing by (name,
// size). A second pass decides whether it is a rename or genuinely new.
constexpr uint16_t FIRST_SEEN_UNRESOLVED = 0xFFFF;

// State threaded through the recursive walk. Passed by reference rather than
// captured, so the walk stays a plain function and its stack frame stays small.
struct WalkState {
  HalFile stage;
  char* nameBuf = nullptr;
  StagedEntry* stagedEntry = nullptr;
  uint16_t books = 0;
  uint16_t folderId = 0;
  uint32_t folderBytes = 0;
  uint32_t nameBytes = 0;
  uint16_t nextFirstSeen = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint64_t* dedupKeys = nullptr;
  bool dedupDegraded = false;
  bool failed = false;
  bool readMetadata = false;
  uint16_t enriched = 0;
  HalFile folders;  // folder section, staged separately then copied in
  // Books the previous index knew. Empty on a first build, in which case every
  // book is new and gets a fresh firstSeen.
  PriorEntry* prior = nullptr;
  uint16_t priorCount = 0;
  uint16_t reused = 0;
  uint32_t serviceUnits = 0;
};

// Bound to shownTitle when a book told us nothing. A `std::string()` temporary
// in that ternary would copy the title on every book that DID tell us something,
// because the two branches have different value categories.
const std::string kNoTitle;

// Find the previous record for this exact file. Linear because the array is at
// most a few hundred entries and this runs once per book during a walk that is
// already dominated by SD seeks.
int findPrior(WalkState& st, const uint32_t nameHash, const uint32_t size) {
  for (uint16_t i = 0; i < st.priorCount; i++) {
    serviceBuilder(st.serviceUnits);
    if (!st.prior[i].matched && st.prior[i].nameHash == nameHash && st.prior[i].size == size) return i;
  }
  return -1;
}

// parentBasename and depth are gone with the folder-as-author rule they served:
// nothing about a book's surroundings names its author any more.
bool stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize, const uint16_t folderId,
                 const std::string& fullPath) {
  StagedEntry& entry = *st.stagedEntry;
  memset(&entry, 0, sizeof(entry));
  // The filename is a fallback for the title and nothing else: no parsing, and
  // never an author. Per review on #2885 -- no other reader parses filenames,
  // and a name pulled out of one by pattern is a guess wearing a fact's clothes.
  std::string title = stemOf(name);
  std::string author;
  std::string series;
  uint16_t seriesIndex = SERIES_INDEX_NONE;
  bool titleFromBook = false;
  bool authorFromBook = false;

  // Prefer the reader's existing cache. For an unopened book, loadMetadata()
  // reuses the same EPUB parser but stops before the manifest, so this never
  // builds spine, TOC, CSS, cover, or section caches during the library walk.
  if (st.readMetadata && FsHelpers::hasEpubExtension(name)) {
    Epub epub(fullPath, CACHE_DIR);
    std::string bookTitle;
    std::string seriesIndexText;
    if (epub.loadMetadata(bookTitle, author, series, seriesIndexText)) {
      if (!bookTitle.empty()) {
        title = std::move(bookTitle);
        titleFromBook = true;
      }
      authorFromBook = !author.empty();
      seriesIndex = parseSeriesIndex(seriesIndexText);
    }
    if (!titleFromBook && !authorFromBook) LOG_DBG("LIBIDX", "no metadata for %s", fullPath.c_str());
  }
  // Exporters write "Unknown" into dc:creator often enough that treating it as
  // a person would put a fictional author at the top of the shelf. fold() already
  // lowercases and trims, so recognising it is the one comparison @Uri-Tauber
  // asked it to cost.
  if (!author.empty() && fold(author) == "unknown") {
    author.clear();
    authorFromBook = false;
  }

  if (titleFromBook || authorFromBook) st.enriched++;

  // An absent author is a fact, not a gap to fill: the row joins the Unknown
  // group rather than borrowing a name from its surroundings.
  const std::string folded = fold(title, true);
  const std::string key = authorKey(author);

  entry.record.nameOff = st.nameBytes;
  entry.record.fileSize = fileSize;

  // Reuse the arrival order this book already had. Without this every rebuild
  // renumbers the whole library in disk-walk order, and "Recently added" silently
  // becomes "whatever order the card enumerates in".
  const int priorIndex = findPrior(st, fnv1a32(name.data(), name.size()), fileSize);
  if (priorIndex >= 0) {
    st.prior[priorIndex].matched = true;
    entry.record.firstSeen = st.prior[priorIndex].firstSeen;
    st.reused++;
  } else {
    // Might be a rename rather than a new book; resolved after the walk, when
    // the set of genuinely unmatched previous entries is known.
    entry.record.firstSeen = FIRST_SEEN_UNRESOLVED;
  }
  entry.record.folderId = folderId;
  // In range: walk() skips names longer than STAGE_NAME_BYTES before staging.
  // readPath() rebuilds the file path from this slot, so a clamp here would
  // stage a row that renders but cannot open.
  entry.record.nameLen = static_cast<uint8_t>(name.size());
  // Only stored when the book actually told us something; otherwise the row falls
  // back to the filename and nothing is duplicated.
  const std::string& shownTitle = titleFromBook ? title : kNoTitle;
  entry.titleLen = static_cast<uint8_t>(std::min<size_t>(shownTitle.size(), STAGE_NAME_BYTES));
  if (entry.titleLen > 0) memcpy(entry.title, shownTitle.data(), entry.titleLen);
  const size_t foldBytes = std::min(folded.size(), CLIX_FOLD_BYTES);
  entry.record.foldLen = static_cast<uint8_t>(utf8SafeTruncateBuffer(folded.data(), static_cast<int>(foldBytes)));
  entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
  memcpy(entry.record.fold, folded.data(), entry.record.foldLen);
  memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  memcpy(entry.name, name.data(), entry.record.nameLen);

  const std::string displayAuthor = cleanPersonName(author);
  entry.authorLen = static_cast<uint8_t>(std::min(displayAuthor.size(), STAGE_AUTHOR_BYTES));
  memcpy(entry.author, displayAuthor.data(), entry.authorLen);

  const int seriesBytes = static_cast<int>(std::min(series.size(), CLIX_SERIES_NAME_BYTES));
  entry.series.len = static_cast<uint8_t>(utf8SafeTruncateBuffer(series.data(), seriesBytes));
  memcpy(entry.series.name, series.data(), entry.series.len);
  // A position without a series is meaningless, and keeping one would let a
  // stray group-position order the standalones against each other.
  entry.series.index = entry.series.len > 0 ? seriesIndex : SERIES_INDEX_NONE;

  if (st.stage.write(reinterpret_cast<const uint8_t*>(&entry), STAGE_STRIDE) != STAGE_STRIDE) {
    LOG_ERR("LIBIDX", "record stage write failed: %s", fullPath.c_str());
    st.failed = true;
    return false;
  }
  st.nameBytes += entry.record.nameLen;
  st.books++;
  return true;
}

void walk(WalkState& st, const std::string& path, const int depth) {
  if (st.failed || depth > LIBRARY_MAX_DEPTH || st.books >= CLIX_MAX_RECORDS) return;

  HalFile dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  dir.rewindDirectory();

  // Identities already staged from THIS directory. A damaged FAT can enumerate
  // the same entry twice; the second one would be a phantom book the user cannot
  // open.
  //
  // Hashes because the names do not fit. Two thousand books in one flat folder —
  // the figure LibraryFormat.h cites as the case to survive — is about 360 KB of
  // std::string against a device that has under 200 KB free, and std::vector grows
  // by throwing, so the failure is abort() and a reboot loop on every rebuild
  // rather than a degraded scan. The one fixed buffer is allocated fallibly by
  // buildLibraryIndex(), reused for each directory, and never grows.
  //
  // Keyed on (name hash, size) packed into 64 bits, not the hash alone. Two
  // different books colliding in 32 bits AND sharing a byte-exact size is
  // implausible where a bare hash collision is merely unlikely, and the cost of
  // being wrong is a real book silently missing from the shelf — the failure
  // hardest to notice and hardest to explain.
  uint16_t seenCount = 0;

  bool folderEmitted = false;
  uint16_t myFolderId = 0;

  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    serviceBuilder(st.serviceUnits);
    if (st.failed || st.books >= CLIX_MAX_RECORDS) {
      entry.close();
      break;
    }
    st.nameBuf[0] = '\0';
    entry.getName(st.nameBuf, NAME_BUF_SIZE);
    const bool isDir = entry.isDirectory();
    const uint32_t size = isDir ? 0 : static_cast<uint32_t>(entry.fileSize());
    entry.close();

    if (st.nameBuf[0] == '\0' || isHiddenOrSidecar(st.nameBuf)) continue;
    const std::string name(st.nameBuf);

    if (isDir) continue;
    if (!isBookName(name)) continue;

    // A zero-length book is a dangling directory entry: the name enumerates but
    // the contents do not exist. Counted rather than silently dropped.
    if (size == 0) {
      st.unreadableSkipped++;
      continue;
    }
    // The index stores the name and the folder path behind one length byte
    // each, and readPath() reconstructs "<folder>/<name>" from those bytes. An
    // entry that does not fit is skipped and counted, never clamped: a clamped
    // name still renders on the shelf but reconstructs to a path that cannot
    // open, and a byte-level cut is not even valid UTF-8. The limit is real —
    // FAT allows 255 UTF-16 units, so a long Cyrillic or CJK filename can run
    // to ~765 UTF-8 bytes.
    if (name.size() > STAGE_NAME_BYTES || path.size() > FOLDER_PATH_BYTES) {
      st.unreadableSkipped++;
      continue;
    }
    const uint64_t key = (static_cast<uint64_t>(fnv1a32(name.data(), name.size())) << 32) | size;
    if (st.dedupKeys != nullptr) {
      bool duplicate = false;
      for (uint16_t i = 0; i < seenCount; i++) {
        serviceBuilder(st.serviceUnits);
        if (st.dedupKeys[i] == key) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        st.duplicatesDropped++;
        continue;
      }
      if (seenCount < LIBRARY_MAX_DEDUP_KEYS) {
        st.dedupKeys[seenCount++] = key;
      } else if (!st.dedupDegraded) {
        LOG_INF("LIBIDX", "duplicate detection capped at %u entries in %s",
                static_cast<unsigned>(LIBRARY_MAX_DEDUP_KEYS), path.c_str());
        st.dedupDegraded = true;
      }
    }
    if (!folderEmitted) {
      // Folders are emitted lazily, so only directories that actually hold a
      // book get an id and the ids stay dense.
      myFolderId = st.folderId;
      // In range: entries whose folder path exceeds FOLDER_PATH_BYTES were
      // skipped above, so no book reaches this line with an overlong path.
      const uint8_t pathLen = static_cast<uint8_t>(path.size());
      if (st.folders.write(&pathLen, 1) != 1 ||
          st.folders.write(reinterpret_cast<const uint8_t*>(path.data()), pathLen) != pathLen) {
        LOG_ERR("LIBIDX", "folder stage write failed: %s", path.c_str());
        st.failed = true;
        break;
      }
      st.folderBytes += 1u + pathLen;
      st.folderId++;
      folderEmitted = true;
    }
    if (!stageRecord(st, name, size, myFolderId, joinLibraryPath(path, name))) break;
  }
  dir.close();

  // Walk subdirectories in one second pass. Close the parent around recursion so
  // only one directory reader is active, then reopen it at the saved position.
  HalFile parent = Storage.open(path.c_str());
  if (!parent || !parent.isDirectory()) {
    if (parent) parent.close();
    st.unreadableSkipped++;
    return;
  }
  parent.rewindDirectory();
  for (HalFile entry = parent.openNextFile(); entry; entry = parent.openNextFile()) {
    serviceBuilder(st.serviceUnits);
    st.nameBuf[0] = '\0';
    entry.getName(st.nameBuf, NAME_BUF_SIZE);
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!isDir || st.nameBuf[0] == '\0' || isHiddenOrSidecar(st.nameBuf)) continue;

    const std::string sub(st.nameBuf);
    const size_t resumePosition = parent.position();
    parent.close();
    walk(st, joinLibraryPath(path, sub), depth + 1);
    if (st.failed || st.books >= CLIX_MAX_RECORDS) return;

    parent = Storage.open(path.c_str());
    if (!parent || !parent.isDirectory() || !parent.seekSet(resumePosition)) {
      if (parent) parent.close();
      st.unreadableSkipped++;
      return;
    }
  }
}

// Shared by the offset and write passes so the name-blob layout has one source of
// truth.
uint32_t blobBytesFor(const StagedEntry& entry, const StagedEntry& canonical) {
  return entry.record.nameLen + 1u + canonical.authorLen + 1u + entry.titleLen;
}

// Group the library into series and settle the order the shelf reads them in.
//
// Fills `seriesOrderOf` (display row -> position in title order), `seriesIdOf`
// (title-order position -> series id, CLIX_SERIES_NONE for a standalone) and
// `seriesIndexOf` (its position within that series), all sized n.
//
// Runs BEFORE the author and arrival sorts and releases its key array on the way
// out, so the passes never hold their arrays at once: the peak stays the largest
// of them rather than their sum.
//
// Returns false only when that array cannot be allocated, which is not fatal to
// the build. The caller then writes an index carrying no series — a shelf that
// reads as an untagged library and is short one tab, where failing the whole
// emit would cost the reader every other order as well.
bool buildSeriesOrder(HalFile& stage, const uint16_t* order, const uint16_t n, uint16_t* seriesOrderOf,
                      uint16_t* seriesIdOf, uint16_t* seriesIndexOf, uint16_t& seriesCount,
                      uint16_t& knownSeriesCount) {
  seriesCount = 0;
  knownSeriesCount = 0;
  for (uint16_t i = 0; i < n; i++) {
    seriesIdOf[i] = CLIX_SERIES_NONE;
    seriesIndexOf[i] = SERIES_INDEX_NONE;
    seriesOrderOf[i] = i;
  }
  if (n == 0) return true;

  auto keys = makeUniqueNoThrow<SeriesSortKey[]>(n);
  if (!keys) {
    LOG_ERR("LIBIDX", "OOM: %u series keys", static_cast<unsigned>(n));
    return false;
  }

  for (uint16_t i = 0; i < n; i++) {
    SeriesSortKey& key = keys[i];
    key.ordinal = i;

    StagedSeries staged;
    stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE + offsetof(StagedEntry, series));
    if (stage.read(reinterpret_cast<uint8_t*>(&staged), sizeof(staged)) != static_cast<int>(sizeof(staged))) {
      // A card that failed mid-rebuild would otherwise group this book under
      // whatever prefix of the name did arrive. Treat it as nameless instead, so
      // it sorts with the standalones rather than inventing a series.
      memset(key.key, 0xFF, sizeof(key.key));
      continue;
    }

    const size_t len = std::min<size_t>(staged.len, CLIX_SERIES_NAME_BYTES);
    // Folding with the article stripped is what puts "The Stormlight Archive"
    // under S, where a reader looking along a shelf expects it.
    const std::string folded = len > 0 ? fold(std::string_view(staged.name, len), true) : std::string();
    if (folded.empty()) {
      // 0xFF outranks every folded byte, so standalones land after every series
      // and knownSeriesCount is simply where they begin. A name that folds away
      // to nothing — punctuation, or a stray space — is no name at all. That
      // includes names written entirely in a script fold() cannot map, which are
      // filed with the standalones, so the demotion is said out loud.
      if (len > 0) {
        LOG_DBG("LIBIDX", "series \"%.*s\" folds to nothing; filed standalone", static_cast<int>(len), staged.name);
      }
      memset(key.key, 0xFF, sizeof(key.key));
      continue;
    }

    // Kept here so the emit pass need not seek the staging file again for a
    // value this loop already has in hand.
    seriesIndexOf[i] = staged.index;

    memset(key.key, 0, sizeof(key.key));
    memcpy(key.key, folded.data(), std::min(folded.size(), SERIES_KEY_NAME_BYTES));
    // Big-endian, so the digest orders arbitrarily but only ever between names
    // that already agree over every byte the shelf sorts on.
    const uint32_t digest = fnv1a32(folded.data(), folded.size());
    for (size_t b = 0; b < SERIES_KEY_HASH_BYTES; b++) {
      const unsigned shift = 8u * static_cast<unsigned>(SERIES_KEY_HASH_BYTES - 1 - b);
      key.key[SERIES_KEY_NAME_BYTES + b] = static_cast<char>((digest >> shift) & 0xFF);
    }
    // Big-endian into the tail so memcmp orders positions numerically.
    key.key[SERIES_KEY_ID_BYTES] = static_cast<char>(staged.index >> 8);
    key.key[SERIES_KEY_ID_BYTES + 1] = static_cast<char>(staged.index & 0xFF);
    knownSeriesCount++;
  }

  if (n > 1) std::sort(keys.get(), keys.get() + n, seriesKeyLess);

  for (uint16_t k = 0; k < n; k++) seriesOrderOf[k] = keys[k].ordinal;
  // Ids are handed out along the sorted array, so the table can be written
  // straight through in one pass. Each group is a run of equal identity bytes,
  // which the digest is what makes trustworthy: on the name bytes alone two
  // series with a shared prefix would land in one run. Id order therefore follows
  // this key — alphabetical over the first 16 folded bytes, then digest order —
  // which is very nearly but not exactly alphabetical, and nothing relies on it
  // being.
  for (uint16_t k = 0; k < knownSeriesCount; k++) {
    if (k == 0 || !sameSeriesGroup(keys[k], keys[k - 1])) seriesCount++;
    seriesIdOf[keys[k].ordinal] = static_cast<uint16_t>(seriesCount - 1);
  }
  return true;
}

bool emitIndex(const char* folderStagePath, WalkState& st, const uint16_t* order, const uint16_t* resolvedFirstSeen,
               BuildStats& stats) {
  const uint16_t n = st.books;
  uint32_t serviceUnits = 0;

  auto arrivalOrder = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (!arrivalOrder) {
    LOG_ERR("LIBIDX", "arrival order array alloc failed");
    return false;
  }

  HalFile stage;
  if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) return false;

  // The series pass runs first, because seriesCount decides where every section
  // after the permutations starts and layoutSections needs it below.
  //
  // Capped like the author sort: a library too big to rank is one whose shelf
  // already hides its sort controls, so a library that cannot be ordered by
  // author cannot be ordered by series either — and this array is 24 bytes a
  // book against 14 for the others.
  //
  // Skipped when the walk read no metadata: series come from metadata and
  // nowhere else, so every staged name is empty and the pass would spend n
  // seeks, n 64-byte reads and a sort proving it.
  auto seriesOrderOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  auto seriesIdOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  auto seriesIndexOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  uint16_t seriesCount = 0;
  uint16_t knownSeriesCount = 0;
  if (seriesOrderOf && seriesIdOf && seriesIndexOf && st.readMetadata && n <= LIBRARY_MAX_SORTED) {
    if (!buildSeriesOrder(stage, order, n, seriesOrderOf.get(), seriesIdOf.get(), seriesIndexOf.get(), seriesCount,
                          knownSeriesCount)) {
      seriesCount = 0;
      knownSeriesCount = 0;
    }
  } else if (seriesOrderOf && seriesIdOf && seriesIndexOf) {
    for (uint16_t i = 0; i < n; i++) {
      seriesOrderOf[i] = i;
      seriesIdOf[i] = CLIX_SERIES_NONE;
      seriesIndexOf[i] = SERIES_INDEX_NONE;
    }
  }
  // The three degrade together: a series order without the ids behind it would
  // draw headings out of a table nothing filled in.
  if (!seriesOrderOf || !seriesIdOf || !seriesIndexOf) {
    seriesOrderOf.reset();
    seriesIdOf.reset();
    seriesIndexOf.reset();
    seriesCount = 0;
    knownSeriesCount = 0;
  }
  stats.series = seriesCount;
  stats.inSeries = knownSeriesCount;

  ClixHeader header{};
  memcpy(header.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  header.formatVersion = CLIX_FORMAT_VERSION;
  header.foldVersion = CLIX_FOLD_VERSION;
  header.bookCount = n;
  header.folderCount = st.folderId;
  header.nextFirstSeen = st.nextFirstSeen;
  header.seriesCount = seriesCount;
  header.knownSeriesCount = knownSeriesCount;
  // Placeholder only. Degradations are known after the sorts have run.
  header.flags = st.readMetadata ? CLIX_FLAG_USED_METADATA : 0;
  // The blob is the LAST section, so its size affects only selfSize — every
  // section offset is already fixed by the counts. Lay out with a placeholder
  // and correct selfSize once the blob has actually been written, since the
  // author spelling each record ends up carrying is not known until the
  // one-spelling-per-person pass has run.
  layoutSections(header, st.folderBytes, 0);

  HalFile out;
  if (!Storage.openFileForWrite("LIBIDX", NEW_PATH, out)) {
    stage.close();
    return false;
  }

  // Returns false rather than spinning. A full card makes write() return 0, and
  // the old loop never advanced past it — the device simply hung mid-rebuild with
  // no message, which is worse than any error.
  bool ioFailed = false;
  // Every final-index write goes through here. A short write on a full card
  // leaves a file that still passes the header check when the header describes
  // what was intended rather than what landed.
  const auto put = [&out, &ioFailed](const void* data, const size_t len) {
    if (ioFailed) return;
    if (out.write(static_cast<const uint8_t*>(data), len) != static_cast<int>(len)) ioFailed = true;
  };
  const auto padTo = [&out, &ioFailed, &serviceUnits](const uint32_t target) {
    if (ioFailed) return;
    static const uint8_t zeros[64] = {0};
    while (out.position() < target) {
      serviceBuilder(serviceUnits);
      const uint32_t gap = target - static_cast<uint32_t>(out.position());
      const size_t want = std::min<uint32_t>(gap, sizeof(zeros));
      if (out.write(zeros, want) != static_cast<int>(want)) {
        ioFailed = true;
        return;
      }
    }
  };
  const auto readStageAt = [&stage, &ioFailed](const uint64_t offset, void* data, const size_t len) {
    if (ioFailed) return false;
    if (!stage.seekSet(offset) || stage.read(reinterpret_cast<uint8_t*>(data), len) != static_cast<int>(len)) {
      LOG_ERR("LIBIDX", "record stage read failed at %u", static_cast<unsigned>(offset));
      ioFailed = true;
      return false;
    }
    return true;
  };

  // Header placeholder; rewritten below once the sorts have run.
  put(&header, sizeof(header));
  padTo(header.folderStart);

  {
    HalFile folders;
    if (Storage.openFileForRead("LIBIDX", folderStagePath, folders)) {
      uint8_t buf[256];
      uint32_t copied = 0;
      // read() returns int: a -1 error must fail the emit, not wrap into a
      // huge unsigned length.
      int got = 0;
      while ((got = folders.read(buf, sizeof(buf))) > 0) {
        serviceBuilder(serviceUnits);
        put(buf, static_cast<size_t>(got));
        copied += static_cast<uint32_t>(got);
      }
      if (got < 0) ioFailed = true;
      folders.close();
      if (copied != st.folderBytes) {
        LOG_ERR("LIBIDX", "folder stage truncated: read %u of %u bytes", static_cast<unsigned>(copied),
                static_cast<unsigned>(st.folderBytes));
        ioFailed = true;
      }
    } else {
      // Ignoring this would publish an all-zero folder section: selfSize still
      // matches, so the index validates, and readPath() then fails for every
      // book with nothing left to trigger a self-repair.
      LOG_ERR("LIBIDX", "folder stage unreadable: %s", folderStagePath);
      ioFailed = true;
    }
  }
  padTo(header.recordStart);
  if (ioFailed) {
    LOG_ERR("LIBIDX", "emit failed while copying the folder stage");
    stage.close();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }

  // Author order has to be known BEFORE the records are written because its
  // permutation section is emitted first.
  // Capped like the title sort: the 14-byte author keys do not fit at the
  // 4096-record ceiling, so larger libraries keep walk order instead.
  const bool rankable = n <= LIBRARY_MAX_SORTED;
  auto authorSort = rankable ? makeUniqueNoThrow<SortKey[]>(n == 0 ? 1 : n) : nullptr;
  if (authorSort) {
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      ClixRecord r{};
      if (!readStageAt(static_cast<uint64_t>(order[i]) * STAGE_STRIDE, &r, sizeof(r))) break;
      if (r.authorKeyLen == 0) {
        // 0xFF outranks every folded byte, so unknown authors land at the end.
        memset(authorSort[i].key, 0xFF, sizeof(authorSort[i].key));
      } else {
        memset(authorSort[i].key, 0, sizeof(authorSort[i].key));
        memcpy(authorSort[i].key, r.authorKey, std::min<size_t>(r.authorKeyLen, sizeof(authorSort[i].key)));
      }
      authorSort[i].ordinal = i;
    }
    if (!ioFailed) {
      if (n > 1) {
        delay(1);
        std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
        delay(1);
      }
    }
  } else {
    stats.ranksDegraded = true;
  }

  // --- one spelling per person --------------------------------------------
  //
  // The author KEY already merges "Xun, Lu", "Lu Xun_" and
  // "Lu Xun [Xun, Lu]" into one identity, because its tokens are
  // sorted. The displayed STRING is still whatever each filename happened to
  // carry, so one person appears under several spellings in the same list.
  //
  // Fix: within each key group show the spelling that occurs most often, ties
  // broken by the shortest and then alphabetically. It never invents or reorders
  // a name — it picks one of the strings that actually exist — which is what
  // keeps "Lu Xun" and "Natsume Soseki" safe from a forename/surname rule
  // that would confidently get them backwards.
  //
  // authorSort is already grouped: books by one person are contiguous in it. So
  // this is one walk over the runs, holding only the current run's spellings.
  std::unique_ptr<uint16_t[]> canonicalFrom;
  std::unique_ptr<SpellingSlot[]> spellingScratch;
  if (authorSort && n > 1) {
    canonicalFrom = makeUniqueNoThrow<uint16_t[]>(n);
  }
  if (canonicalFrom) {
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      canonicalFrom[i] = i;
    }
    spellingScratch = makeUniqueNoThrow<SpellingSlot[]>(MAX_AUTHOR_SPELLINGS);
  } else if (authorSort && n > 1) {
    LOG_ERR("LIBIDX", "canonical author array alloc failed; author order degraded");
    stats.ranksDegraded = true;
  }
  if (canonicalFrom && !spellingScratch) {
    LOG_ERR("LIBIDX", "author spelling scratch alloc failed; spelling harmonisation skipped");
    stats.ranksDegraded = true;
  }
  if (!ioFailed && canonicalFrom && spellingScratch && authorSort && n > 1) {
    uint16_t runStart = 0;
    while (runStart < n) {
      serviceBuilder(serviceUnits);
      uint16_t runEnd = runStart + 1;
      while (runEnd < n &&
             memcmp(authorSort[runEnd].key, authorSort[runStart].key, sizeof(authorSort[runStart].key)) == 0) {
        serviceBuilder(serviceUnits);
        runEnd++;
      }
      // A run of one has nothing to reconcile, and the unknown-author run (key
      // all 0xFF) must not be collapsed onto one arbitrary empty string.
      const bool unknownRun = static_cast<unsigned char>(authorSort[runStart].key[0]) == 0xFF;
      if (!unknownRun && runEnd - runStart > 1) {
        // Each author read ONCE, then counted in RAM. The first version re-read
        // the whole run for every member of it — k² reads of 768 bytes for a
        // number that k reads can produce — and an author with twenty books cost
        // four hundred SD reads to decide one string.
        //
        // Bounded by DISTINCT spellings rather than by run length, which is the
        // point: one person has two or three spellings on a real card, however
        // many books they wrote, so this holds a handful of short strings instead
        // of one per book.
        uint8_t spellingCount = 0;

        for (uint16_t a = runStart; a < runEnd; a++) {
          serviceBuilder(serviceUnits);
          const uint16_t ord = authorSort[a].ordinal;
          uint8_t len = 0;
          if (!readStageAt(static_cast<uint64_t>(order[ord]) * STAGE_STRIDE + offsetof(StagedEntry, authorLen), &len,
                           sizeof(len)))
            break;
          if (len == 0) continue;

          char buf[STAGE_AUTHOR_BYTES];
          const size_t want = std::min<size_t>(len, sizeof(buf));
          if (!readStageAt(static_cast<uint64_t>(order[ord]) * STAGE_STRIDE + offsetof(StagedEntry, author), buf, want))
            break;
          bool merged = false;
          for (uint8_t i = 0; i < spellingCount; i++) {
            SpellingSlot& sp = spellingScratch[i];
            if (sp.len == want && memcmp(sp.text, buf, want) == 0) {
              sp.count++;
              merged = true;
              break;
            }
          }
          // A hard cap so a card full of near-identical spellings cannot grow this
          // without bound. Sixteen is far past anything real; beyond it the vote
          // simply decides among the first sixteen.
          if (!merged && spellingCount < MAX_AUTHOR_SPELLINGS) {
            SpellingSlot& sp = spellingScratch[spellingCount++];
            memcpy(sp.text, buf, want);
            sp.ordinal = ord;
            sp.count = 1;
            sp.len = static_cast<uint8_t>(want);
          }
        }

        uint16_t bestOrdinal = authorSort[runStart].ordinal;
        int bestScore = -1;
        size_t bestLen = 0;
        const char* bestText = nullptr;
        for (uint8_t i = 0; i < spellingCount; i++) {
          const SpellingSlot& sp = spellingScratch[i];
          const bool better = sp.count > bestScore || (sp.count == bestScore && sp.len < bestLen) ||
                              (sp.count == bestScore && sp.len == bestLen &&
                               (bestText == nullptr || memcmp(sp.text, bestText, sp.len) < 0));
          if (better) {
            bestScore = sp.count;
            bestLen = sp.len;
            bestText = sp.text;
            bestOrdinal = sp.ordinal;
          }
        }
        for (uint16_t a = runStart; a < runEnd; a++) {
          serviceBuilder(serviceUnits);
          canonicalFrom[authorSort[a].ordinal] = bestOrdinal;
        }
      }
      runStart = runEnd;
    }
  }

  // --- re-sort by surname --------------------------------------------------
  //
  // The pass above had to run in authorKey order, because that is what puts one
  // author's books in a single run for the spelling vote. But authorKey sorts a
  // name's WORDS — the property that lets "Victor Hugo" and "Hugo Victor" be
  // recognised as one person — so ordering by it files Herman Melville under B.
  //
  // Now that every book carries its canonical display name, the shelf is ordered
  // by surname, as a library would. Keying off the canonical name rather than the
  // raw one is what keeps a group whole: all of a group's books resolve to the
  // same string, so they cannot split across two places.
  if (!ioFailed && authorSort && canonicalFrom && n > 1) {
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      // canonicalFrom holds TITLE-order positions, and the staging file is keyed
      // by walk order — order[] is the map between them. Reading staging with the
      // title position directly fetches an unrelated book, which is what split
      // John Scalzi into two groups and left the shelf in no order at all.
      const uint16_t src = order[canonicalFrom[i]];
      uint8_t authorLen = 0;
      char author[STAGE_AUTHOR_BYTES] = {};
      if (!readStageAt(static_cast<uint64_t>(src) * STAGE_STRIDE + offsetof(StagedEntry, authorLen), &authorLen,
                       sizeof(authorLen)))
        break;
      if (authorLen > 0) {
        if (!readStageAt(static_cast<uint64_t>(src) * STAGE_STRIDE + offsetof(StagedEntry, author), author,
                         std::min<size_t>(authorLen, sizeof(author))))
          break;
      }

      const std::string key = authorLen == 0 ? std::string() : surnameKey(std::string_view(author, authorLen));
      if (key.empty()) {
        // 0xFF outranks every folded byte, so unknown authors stay at the end.
        memset(authorSort[i].key, 0xFF, sizeof(authorSort[i].key));
      } else {
        memset(authorSort[i].key, 0, sizeof(authorSort[i].key));
        memcpy(authorSort[i].key, key.data(), std::min(key.size(), sizeof(authorSort[i].key)));
      }
      authorSort[i].ordinal = i;
    }
    if (!ioFailed) {
      delay(1);
      std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
      delay(1);
    }
  }

  // --- arrival order -------------------------------------------------------
  //
  // firstSeen values now come from the PREVIOUS index, so they are no longer a
  // dense sequence in walk order: a rebuild reuses each book's original number
  // and only hands out new ones for books it has never seen. The arrival order has
  // to be SORTED rather than assumed, or "Recently added" silently degrades into
  // "the order the card enumerates in" — which is exactly the bug reconciliation
  // exists to prevent.
  if (rankable) {
    for (uint16_t i = 0; i < n; i++) arrivalOrder[i] = i;
    if (n > 1) {
      delay(1);
      std::sort(arrivalOrder.get(), arrivalOrder.get() + n,
                [order, resolvedFirstSeen](const uint16_t a, const uint16_t b) {
                  const uint16_t aSeen = resolvedFirstSeen[order[a]];
                  const uint16_t bSeen = resolvedFirstSeen[order[b]];
                  return aSeen < bSeen || (aSeen == bSeen && a < b);
                });
      delay(1);
    }
  } else {
    // Without sorting, preserve walk order by mapping each staging ordinal back
    // to its position in the title-ordered record section.
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      arrivalOrder[order[i]] = i;
    }
    LOG_INF("LIBIDX", "%u books over the %u sort cap: author and arrival order degraded", static_cast<unsigned>(n),
            static_cast<unsigned>(LIBRARY_MAX_SORTED));
    stats.ranksDegraded = true;
  }

  if (ioFailed) {
    LOG_ERR("LIBIDX", "emit failed while reading the record stage");
    stage.close();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }

  // Records, in title order, with both ranks and the name offset filled in.
  //
  // nameOff MUST be recomputed here. The walk assigns offsets in discovery
  // order, but the name blob below is written in title order, so a staged offset
  // points at whatever name happened to be staged at that position — which
  // renders as the tail of one name glued to the head of the next.
  // One pair of staging buffers on the heap, reused by both emit loops. As
  // locals they were 768 bytes each, so 1.5 KB of stack inside a function running
  // on a 4 KB task — the kind of margin that survives a test library and fails on
  // someone else's. On the heap the allocation is checked; on the stack an
  // overflow is a silent corruption.
  auto staged = makeUniqueNoThrow<StagedEntry[]>(2);
  if (!staged) {
    LOG_ERR("LIBIDX", "staging buffers alloc failed");
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }
  StagedEntry& entry = staged[0];
  StagedEntry& canonical = staged[1];

  // put()'s rule, applied to the reads. A short read leaves the previous book's
  // bytes in the buffer, so the emit would write a duplicate row — and since the
  // duplicate is internally consistent, written == selfSize still holds and the
  // corrupt index would pass validation.
  const auto fetch = [&readStageAt](const uint16_t stagingIndex, StagedEntry& dest) {
    return readStageAt(static_cast<uint64_t>(stagingIndex) * STAGE_STRIDE, &dest, STAGE_STRIDE);
  };

  uint32_t nameCursor = 0;
  for (uint16_t i = 0; i < n; i++) {
    serviceBuilder(serviceUnits);
    if (!fetch(order[i], entry)) break;
    entry.record.nameOff = nameCursor;
    // The blob holds the basename, then one length byte, then the chosen author
    // spelling, then the title. Keeping them adjacent means no second offset has
    // to live in the record, which is exactly full at 128 bytes.
    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    if (!fetch(order[from], canonical)) break;
    nameCursor += blobBytesFor(entry, canonical);
    if (resolvedFirstSeen) entry.record.firstSeen = resolvedFirstSeen[order[i]];
    put(&entry.record, sizeof(ClixRecord));
  }
  padTo(header.permStart);

  for (uint16_t k = 0; k < n; k++) {
    serviceBuilder(serviceUnits);
    const uint16_t ordinal = authorSort ? authorSort[k].ordinal : k;
    put(&ordinal, sizeof(ordinal));
  }
  for (uint16_t k = 0; k < n; k++) {
    serviceBuilder(serviceUnits);
    const uint16_t ordinal = arrivalOrder[k];
    put(&ordinal, sizeof(ordinal));
  }
  for (uint16_t k = 0; k < n; k++) {
    serviceBuilder(serviceUnits);
    const uint16_t ordinal = seriesOrderOf ? seriesOrderOf[k] : k;
    put(&ordinal, sizeof(ordinal));
  }
  padTo(header.seriesStart);

  // The series table, in id order — which is the sorted order, since ids were
  // handed out along the sorted array. Each group is a run in seriesOrderOf, so
  // its length is its book count and its first member names it.
  for (uint16_t k = 0; k < knownSeriesCount;) {
    serviceBuilder(serviceUnits);
    const uint16_t first = seriesOrderOf[k];
    const uint16_t id = seriesIdOf[first];
    uint16_t count = 0;
    while (k + count < knownSeriesCount && seriesIdOf[seriesOrderOf[k + count]] == id) count++;

    StagedSeries staged;
    stage.seekSet(static_cast<uint64_t>(order[first]) * STAGE_STRIDE + offsetof(StagedEntry, series));
    // buildSeriesOrder read these same bytes to form this group, so a failure
    // here is the card failing mid-emit. Fail the whole emit rather than write
    // the entry nameless: an empty name draws as a phantom heading mid-shelf for
    // as long as the index lives, while a failed emit keeps the old index.
    if (stage.read(reinterpret_cast<uint8_t*>(&staged), sizeof(staged)) != static_cast<int>(sizeof(staged))) {
      LOG_ERR("LIBIDX", "series %u staged read failed mid-emit", static_cast<unsigned>(id));
      ioFailed = true;
      break;
    }

    ClixSeriesEntry seriesEntry{};
    seriesEntry.bookCount = count;
    seriesEntry.nameLen = static_cast<uint8_t>(std::min<size_t>(staged.len, CLIX_SERIES_NAME_BYTES));
    memcpy(seriesEntry.name, staged.name, seriesEntry.nameLen);
    put(&seriesEntry, sizeof(seriesEntry));
    k += count;
  }
  padTo(header.seriesRefStart);

  // References, parallel to the records, so a record and its series are found
  // the same way.
  for (uint16_t i = 0; i < n; i++) {
    serviceBuilder(serviceUnits);
    ClixSeriesRef ref{};
    ref.seriesId = seriesIdOf ? seriesIdOf[i] : CLIX_SERIES_NONE;
    // From the array the series pass filled, not a fresh seek: it read every
    // book's staged series to build its keys and kept the positions it saw.
    ref.seriesIndex =
        seriesIndexOf && ref.seriesId != CLIX_SERIES_NONE ? seriesIndexOf[i] : static_cast<uint16_t>(SERIES_INDEX_NONE);
    put(&ref, sizeof(ref));
  }
  padTo(header.nameStart);

  uint32_t blobWritten = 0;
  for (uint16_t i = 0; i < n; i++) {
    serviceBuilder(serviceUnits);
    if (!fetch(order[i], entry)) break;
    put(entry.name, entry.record.nameLen);

    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    if (!fetch(order[from], canonical)) break;
    put(&canonical.authorLen, 1);
    if (canonical.authorLen > 0) put(canonical.author, canonical.authorLen);
    put(&entry.titleLen, 1);
    if (entry.titleLen > 0) put(entry.title, entry.titleLen);
    blobWritten += blobBytesFor(entry, canonical);
  }
  header.nameLen = blobWritten;
  header.selfSize = header.nameStart + blobWritten;
  stage.close();

  // Captured HERE, at the end of the data, and not after the header rewrite
  // below: that rewrite seeks back to 0, so asking afterwards reports 64 — the
  // header's own length — and every rebuild looks truncated.
  const uint32_t written = static_cast<uint32_t>(out.position());

  header.flags =
      (stats.ranksDegraded ? CLIX_FLAG_RANKS_DEGRADED : 0) | (stats.dedupDegraded ? CLIX_FLAG_DEDUP_DEGRADED : 0);

  out.seekSet(0);
  put(&header, sizeof(header));
  // The file is only as long as it claims if every write landed. A full card
  // fails them silently, and the result passes the header check while carrying
  // zeros — an index that looks valid and is not.
  const bool sizeMatches = written == header.selfSize;
  out.close();

  if (ioFailed || !sizeMatches) {
    LOG_ERR("LIBIDX", "emit incomplete (I/O %s, size %u vs %u) — keeping the old index", ioFailed ? "failed" : "ok",
            static_cast<unsigned>(written), static_cast<unsigned>(header.selfSize));
    // Leave the previous index alone. A shelf that is a rebuild out of date is
    // worth incomparably more than none at all, and a full card is exactly when
    // the reader can least afford to lose it.
    Storage.remove(NEW_PATH);
    return false;
  }

  // Rename last. The previous index moves to a recoverable backup until the new
  // file owns the live path; a failed rename rolls it back instead of deleting
  // the only usable shelf.
  return installNewIndex();
}

}  // namespace

const char* libraryIndexPath() { return INDEX_PATH; }

bool buildLibraryIndex(const char* rootPath, BuildStats& stats, const bool readMetadata) {
  const uint32_t startMs = millis();
  uint32_t serviceUnits = 0;
  stats = BuildStats{};

  Storage.mkdir(CACHE_DIR);
  if (!recoverInterruptedInstall()) return false;
  Storage.remove(STAGE_PATH);
  const std::string folderStagePath = std::string(STAGE_PATH) + ".f";
  Storage.remove(folderStagePath.c_str());

  auto nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuf) {
    LOG_ERR("LIBIDX", "name buffer alloc failed (%u bytes)", static_cast<unsigned>(NAME_BUF_SIZE));
    return false;
  }

  // The staging record exceeds the task's 256-byte stack budget. Allocate one
  // fallibly for the build and reuse it; static storage would pin scarce DRAM.
  auto stagedEntry = makeUniqueNoThrow<StagedEntry>();
  if (!stagedEntry) {
    LOG_ERR("LIBIDX", "staging record alloc failed (%u bytes)", static_cast<unsigned>(sizeof(StagedEntry)));
    return false;
  }

  auto dedupKeys = makeUniqueNoThrow<uint64_t[]>(LIBRARY_MAX_DEDUP_KEYS);
  if (!dedupKeys) {
    // Duplicate detection is defensive against damaged FAT directory entries.
    // Losing that defence may expose duplicate rows, but it must not make the
    // whole library unavailable when 8 KiB cannot be allocated on a C3.
    LOG_ERR("LIBIDX", "dedup key buffer alloc failed; continuing without duplicate detection");
  }

  // Load what the previous index knew, so the walk can recognise the same books.
  // Failure here is not fatal: the build simply treats every book as new.
  std::unique_ptr<PriorEntry[]> priorList;
  uint16_t priorCount = 0;
  uint16_t nextFirstSeen = 0;
  {
    LibraryIndexFile previous;
    if (previous.openForReconciliation(INDEX_PATH)) {
      nextFirstSeen = previous.header().nextFirstSeen;
      priorCount = previous.bookCount();
      priorList = makeUniqueNoThrow<PriorEntry[]>(priorCount == 0 ? 1 : priorCount);
      if (priorList) {
        uint16_t kept = 0;
        for (uint16_t i = 0; i < priorCount; i++) {
          serviceBuilder(serviceUnits);
          ClixRecord r{};
          std::string name;
          if (!previous.readRecord(i, r) || !previous.readName(r, name)) continue;
          priorList[kept].nameHash = fnv1a32(name.data(), name.size());
          priorList[kept].size = r.fileSize;
          priorList[kept].firstSeen = r.firstSeen;
          priorList[kept].matched = false;
          kept++;
        }
        priorCount = kept;
      } else {
        priorCount = 0;
      }
    }
  }

  WalkState st;
  st.nameBuf = nameBuf.get();
  st.stagedEntry = stagedEntry.get();
  st.dedupKeys = dedupKeys.get();
  st.dedupDegraded = !dedupKeys;
  st.nextFirstSeen = nextFirstSeen;
  st.prior = priorList.get();
  st.priorCount = priorList ? priorCount : 0;
  st.readMetadata = readMetadata;

  if (!Storage.openFileForWrite("LIBIDX", STAGE_PATH, st.stage) ||
      !Storage.openFileForWrite("LIBIDX", folderStagePath, st.folders)) {
    LOG_ERR("LIBIDX", "cannot open staging files");
    if (st.stage) st.stage.close();
    if (st.folders) st.folders.close();
    return false;
  }

  walk(st, rootPath, 0);
  st.stage.close();
  st.folders.close();

  if (st.failed) {
    LOG_ERR("LIBIDX", "staging failed; keeping the previous index");
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }

  // --- second pass: renames, then genuinely new books ----------------------
  //
  // Resolved into RAM, never by rewriting the staging file: openFileForWrite
  // opens with O_TRUNC (SDCardManager.cpp:308), so reopening the staging file to
  // patch it empties it, and every record read afterwards comes back blank.
  // Two bytes per book is a cheaper price than that failure mode.
  //
  // A book that matched nothing by (name, size) is either renamed or new. Match
  // it against the leftover previous entries by SIZE alone: across a real
  // library, two different books sharing a byte-exact size is implausible, and
  // being wrong only costs one book its place in "Recently added" and one
  // re-read. A content hash would settle it properly but would read ~12 KB per
  // book on every single verification, to decide a case that arises when someone
  // renames a file.
  auto resolvedFirstSeen = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!resolvedFirstSeen) {
    LOG_ERR("LIBIDX", "firstSeen array alloc failed");
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  if (st.books > 0) {
    // A stage that cannot be read back fails the BUILD, it does not degrade.
    // The fallback would be firstSeen == 0 for every affected book — wrong in
    // "Recently added" today, and read back as prior truth by the next rebuild,
    // which would then propagate the zeros forever. The previous index survives.
    HalFile read;
    if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, read)) {
      LOG_ERR("LIBIDX", "firstSeen reconciliation: cannot reopen the stage");
      Storage.remove(STAGE_PATH);
      Storage.remove(folderStagePath.c_str());
      return false;
    }
    for (uint16_t i = 0; i < st.books; i++) {
      serviceBuilder(serviceUnits);
      ClixRecord r{};
      if (!read.seekSet(static_cast<uint64_t>(i) * STAGE_STRIDE) ||
          read.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) {
        LOG_ERR("LIBIDX", "firstSeen reconciliation: short read at record %u", static_cast<unsigned>(i));
        read.close();
        Storage.remove(STAGE_PATH);
        Storage.remove(folderStagePath.c_str());
        return false;
      }
      if (r.firstSeen != FIRST_SEEN_UNRESOLVED) {
        resolvedFirstSeen[i] = r.firstSeen;
        continue;
      }
      int renamed = -1;
      for (uint16_t q = 0; q < priorCount; q++) {
        serviceBuilder(serviceUnits);
        if (priorList && !priorList[q].matched && priorList[q].size == r.fileSize) {
          renamed = q;
          break;
        }
      }
      if (renamed >= 0) {
        priorList[renamed].matched = true;
        resolvedFirstSeen[i] = priorList[renamed].firstSeen;
        stats.renamed++;
      } else {
        resolvedFirstSeen[i] = st.nextFirstSeen++;
        stats.added++;
      }
    }
    read.close();
    for (uint16_t q = 0; q < priorCount; q++) {
      serviceBuilder(serviceUnits);
      if (priorList && !priorList[q].matched) stats.removed++;
    }
  }

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.dedupDegraded = st.dedupDegraded;
  stats.unchanged = st.reused;
  stats.enriched = st.enriched;

  // --- title order -----------------------------------------------------------
  // Read the staged fold prefixes back and sort ordinals. Only 14 bytes per book
  // stays resident, and past the cap the index is still complete — just in walk
  // order, which the header records so the screen can say so.
  const bool sortable = st.books <= LIBRARY_MAX_SORTED;
  auto order = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!order) {
    LOG_ERR("LIBIDX", "order array alloc failed (%u books)", static_cast<unsigned>(st.books));
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  for (uint16_t i = 0; i < st.books; i++) {
    serviceBuilder(serviceUnits);
    order[i] = i;
  }

  if (sortable && st.books > 1) {
    auto keys = makeUniqueNoThrow<SortKey[]>(st.books);
    HalFile stage;
    if (keys && Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) {
      for (uint16_t i = 0; i < st.books; i++) {
        serviceBuilder(serviceUnits);
        ClixRecord r{};
        const uint64_t offset = static_cast<uint64_t>(i) * STAGE_STRIDE;
        if (!stage.seekSet(offset) ||
            stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) {
          LOG_ERR("LIBIDX", "title sort: record stage read failed at %u", static_cast<unsigned>(offset));
          stage.close();
          Storage.remove(STAGE_PATH);
          Storage.remove(folderStagePath.c_str());
          return false;
        }
        memset(keys[i].key, 0, sizeof(keys[i].key));
        memcpy(keys[i].key, r.fold, std::min<size_t>(r.foldLen, sizeof(keys[i].key)));
        keys[i].ordinal = i;
      }
      stage.close();
      delay(1);
      std::sort(keys.get(), keys.get() + st.books, sortKeyLess);
      delay(1);
      for (uint16_t i = 0; i < st.books; i++) {
        serviceBuilder(serviceUnits);
        order[i] = keys[i].ordinal;
      }
    } else {
      stats.ranksDegraded = true;
      LOG_ERR("LIBIDX", "sort skipped: key array alloc or stage reopen failed");
    }
  } else if (!sortable) {
    stats.ranksDegraded = true;
    LOG_INF("LIBIDX", "%u books exceeds sort cap %u; index built in walk order", static_cast<unsigned>(st.books),
            static_cast<unsigned>(LIBRARY_MAX_SORTED));
  }

  const bool ok = emitIndex(folderStagePath.c_str(), st, order.get(), resolvedFirstSeen.get(), stats);
  Storage.remove(STAGE_PATH);
  Storage.remove(folderStagePath.c_str());

  stats.walkMs = millis() - startMs;
  LOG_INF("LIBIDX", "%s: %u books, %u folders, %u enriched, %u dup dropped, %u unreadable, metadata %s, %ums",
          ok ? "built" : "FAILED", static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped), readMetadata ? "on" : "off",
          static_cast<unsigned>(stats.walkMs));
  return ok;
}

}  // namespace library
