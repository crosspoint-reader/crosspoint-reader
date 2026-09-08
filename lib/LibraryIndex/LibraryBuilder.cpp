#include "LibraryBuilder.h"

#include <Arduino.h>
#include <BufferedFile.h>
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
constexpr size_t LIBRARY_IO_BUFFER_SIZE = 4096;

// Matches lib/FileIndex's buffer so a name this walk accepts is one the file
// browser could also show.
constexpr size_t NAME_BUF_SIZE = 512;

// One staged entry: the record as far as the filename can fill it, followed by
// the display name. Fixed stride keeps the second pass a seek rather than a scan.
constexpr size_t STAGE_NAME_BYTES = 255;
constexpr size_t STAGE_AUTHOR_BYTES = 128;
// A folder path is stored behind one length byte in the folder section.
constexpr size_t FOLDER_PATH_BYTES = 255;
struct StagedEntry {
  ClixRecord record;
  uint64_t pathHash;
  char name[STAGE_NAME_BYTES];
  // Cleaned source spelling from this book. The spelling actually shown
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
  if (candidate.ioFailed()) {
    LOG_ERR("LIBIDX", "cannot read live index during recovery");
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
  if (candidate.ioFailed()) {
    LOG_ERR("LIBIDX", "cannot read backup index during recovery");
    return false;
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

struct PriorEntry {
  uint64_t pathHash;
  uint32_t fileSize;
  uint16_t firstSeen;
  uint16_t ordinalAndMatched;
};
static_assert(sizeof(PriorEntry) == 16, "prior reconciliation entries must stay at 16 bytes");

constexpr uint16_t PRIOR_MATCHED = 0x8000;
constexpr uint16_t PRIOR_ORDINAL_MASK = 0x7FFF;
static_assert(CLIX_MAX_RECORDS <= PRIOR_MATCHED, "prior ordinal must fit below the matched bit");

uint16_t priorOrdinal(const PriorEntry& entry) { return entry.ordinalAndMatched & PRIOR_ORDINAL_MASK; }

bool priorMatched(const PriorEntry& entry) { return (entry.ordinalAndMatched & PRIOR_MATCHED) != 0; }

void markPriorMatched(PriorEntry& entry) { entry.ordinalAndMatched |= PRIOR_MATCHED; }

bool priorPathLess(const PriorEntry& a, const PriorEntry& b) {
  return a.pathHash < b.pathHash || (a.pathHash == b.pathHash && priorOrdinal(a) < priorOrdinal(b));
}

bool priorSizeLess(const PriorEntry& a, const PriorEntry& b) {
  return a.fileSize < b.fileSize || (a.fileSize == b.fileSize && priorOrdinal(a) < priorOrdinal(b));
}

uint32_t fnv1a32(const char* data, const size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

uint64_t fnv1a64(const char* data, const size_t len) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Sentinel written into a staged record whose book matched no previous path. A
// second pass decides whether it is a rename or genuinely new.
constexpr uint16_t FIRST_SEEN_UNRESOLVED = 0xFFFF;

// State threaded through the recursive walk. Passed by reference rather than
// captured, so the walk stays a plain function and its stack frame stays small.
struct WalkState {
  HalFile stage;
  serialization::BufferedFileWriter* stageOut = nullptr;
  char* nameBuf = nullptr;
  StagedEntry* stagedEntry = nullptr;
  uint16_t books = 0;
  uint16_t folderId = 0;
  uint32_t folderBytes = 0;
  uint16_t nextFirstSeen = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint64_t* dedupKeys = nullptr;
  uint16_t activeDedupCount = 0;
  bool dedupDegraded = false;
  bool failed = false;
  bool readMetadata = false;
  LibraryIndexFile* previous = nullptr;
  BuildStats* stats = nullptr;
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

int findPrior(WalkState& st, const uint64_t pathHash) {
  serviceBuilder(st.serviceUnits);
  if (st.priorCount == 0) return -1;
  PriorEntry* const end = st.prior + st.priorCount;
  PriorEntry* candidate = std::lower_bound(
      st.prior, end, pathHash, [](const PriorEntry& entry, const uint64_t hash) { return entry.pathHash < hash; });
  while (candidate != end && candidate->pathHash == pathHash) {
    if (!priorMatched(*candidate)) return static_cast<int>(candidate - st.prior);
    ++candidate;
  }
  return -1;
}

// parentBasename and depth are gone with the folder-as-author rule they served:
// nothing about a book's surroundings names its author any more.
[[gnu::noinline]] bool stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize,
                                   const uint16_t folderId, const std::string& fullPath,
                                   const uint32_t modificationTime) {
  StagedEntry& entry = *st.stagedEntry;
  memset(&entry, 0, sizeof(entry));
  // The filename is a fallback for the title and nothing else: no parsing, and
  // never an author. Per review on #2885 -- no other reader parses filenames,
  // and a name pulled out of one by pattern is a guess wearing a fact's clothes.
  std::string title = stemOf(name);
  std::string author;
  bool titleFromBook = false;
  bool authorFromBook = false;

  entry.pathHash = fnv1a64(fullPath.data(), fullPath.size());
  const int priorIndex = findPrior(st, entry.pathHash);

  const bool extractionExpected = st.readMetadata && FsHelpers::hasEpubExtension(name);
  const uint8_t expectedStatus = extractionExpected ? CLIX_METADATA_EXTRACTED : CLIX_METADATA_NOT_ATTEMPTED;
  bool reuseMetadata = false;
  ClixRecord priorRecord{};
  if (priorIndex >= 0) {
    if (!st.previous->readRecord(priorOrdinal(st.prior[priorIndex]), priorRecord)) {
      st.failed = true;
      return false;
    }
    reuseMetadata =
        st.prior[priorIndex].fileSize == fileSize && modificationTime != 0 &&
        priorRecord.modificationTime == modificationTime && st.previous->header().foldVersion == CLIX_FOLD_VERSION &&
        st.previous->header().metadataEnabled == st.readMetadata && priorRecord.metadataStatus == expectedStatus;
  }

  if (reuseMetadata) {
    if (!st.previous->readSourceAuthor(priorRecord, author)) {
      st.failed = true;
      return false;
    }
    const bool hasBookTitle = st.previous->readTitle(priorRecord, title);
    if (!hasBookTitle && st.previous->ioFailed()) {
      st.failed = true;
      return false;
    }
    entry.record = priorRecord;
    authorFromBook = !author.empty();
    if (hasBookTitle) {
      titleFromBook = true;
    } else {
      title = stemOf(name);
    }
    st.stats->metadataReused++;
  }

  // Prefer the reader's existing cache. For an unopened book, loadMetadata()
  // reuses the same EPUB parser but stops before the manifest, so this never
  // builds spine, TOC, CSS, cover, or section caches during the library walk.
  if (!reuseMetadata && extractionExpected) {
    st.stats->parsed++;
    Epub epub(fullPath, CACHE_DIR);
    std::string bookTitle;
    if (epub.loadMetadata(bookTitle, author)) {
      entry.record.metadataStatus = CLIX_METADATA_EXTRACTED;
      if (!bookTitle.empty()) {
        title = std::move(bookTitle);
        titleFromBook = true;
      }
      authorFromBook = !author.empty();
    } else {
      entry.record.metadataStatus = CLIX_METADATA_FAILED;
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
  const std::string folded = reuseMetadata ? std::string() : fold(title, true);
  const std::string key = reuseMetadata ? std::string() : authorKey(author);

  entry.record.fileSize = fileSize;
  entry.record.modificationTime = modificationTime;

  // Reuse the arrival order this book already had. Without this every rebuild
  // renumbers the whole library in disk-walk order, and "Recently added" silently
  // becomes "whatever order the card enumerates in".
  if (priorIndex >= 0) {
    markPriorMatched(st.prior[priorIndex]);
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
  if (!reuseMetadata) {
    const size_t foldBytes = std::min(folded.size(), CLIX_FOLD_BYTES);
    entry.record.foldLen = static_cast<uint8_t>(utf8SafeTruncateBuffer(folded.data(), static_cast<int>(foldBytes)));
    entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
    memcpy(entry.record.fold, folded.data(), entry.record.foldLen);
    memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  }
  memcpy(entry.name, name.data(), entry.record.nameLen);

  const std::string displayAuthor = cleanPersonName(author);
  entry.authorLen = static_cast<uint8_t>(std::min(displayAuthor.size(), STAGE_AUTHOR_BYTES));
  memcpy(entry.author, displayAuthor.data(), entry.authorLen);

  st.stageOut->write(&entry, STAGE_STRIDE);
  st.books++;
  return true;
}

struct DedupFrame {
  WalkState& state;
  uint16_t base;
  ~DedupFrame() { state.activeDedupCount = base; }
};

void walk(WalkState& st, const std::string& path, const int depth) {
  if (st.failed || depth > LIBRARY_MAX_DEPTH || st.books >= CLIX_MAX_RECORDS) return;

  const uint16_t dedupBase = st.activeDedupCount;
  const DedupFrame dedupFrame{st, dedupBase};

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
    const uint32_t modificationTime = isDir ? 0 : entry.modificationTime();
    entry.close();

    if (st.nameBuf[0] == '\0' || isHiddenOrSidecar(st.nameBuf)) continue;
    const std::string name(st.nameBuf);

    if (isDir) {
      const size_t resumePosition = dir.position();
      dir.close();
      walk(st, joinLibraryPath(path, name), depth + 1);
      if (st.failed || st.books >= CLIX_MAX_RECORDS) return;

      dir = Storage.open(path.c_str());
      if (!dir || !dir.isDirectory() || !dir.seekSet(resumePosition)) {
        if (dir) dir.close();
        st.unreadableSkipped++;
        return;
      }
      continue;
    }
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
      uint64_t* const end = st.dedupKeys + st.activeDedupCount;
      uint64_t* const slot = std::lower_bound(st.dedupKeys + dedupBase, end, key);
      if (slot != end && *slot == key) {
        st.duplicatesDropped++;
        continue;
      }
      if (st.activeDedupCount < LIBRARY_MAX_DEDUP_KEYS) {
        memmove(slot + 1, slot, static_cast<size_t>(end - slot) * sizeof(*slot));
        *slot = key;
        st.activeDedupCount++;
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
    if (!stageRecord(st, name, size, myFolderId, joinLibraryPath(path, name), modificationTime)) break;
  }
  dir.close();
}

// Shared by the offset and write passes so the name-blob layout has one source of
// truth.
uint32_t blobBytesFor(const StagedEntry& entry, const StagedEntry& canonical) {
  return sizeof(entry.pathHash) + entry.record.nameLen + 1u + canonical.authorLen + 1u + entry.titleLen + 1u +
         entry.authorLen;
}

bool emitIndex(const char* folderStagePath, WalkState& st, const uint16_t* order, const uint16_t* resolvedFirstSeen,
               const bool coreSortsAvailable, BuildStats& stats) {
  const uint16_t n = st.books;
  uint32_t serviceUnits = 0;

  auto arrivalOrder = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (!arrivalOrder) {
    LOG_ERR("LIBIDX", "arrival order array alloc failed");
    return false;
  }

  ClixHeader header{};
  memcpy(header.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  header.formatVersion = CLIX_FORMAT_VERSION;
  header.foldVersion = CLIX_FOLD_VERSION;
  header.bookCount = n;
  header.folderCount = st.folderId;
  header.nextFirstSeen = st.nextFirstSeen;
  header.metadataEnabled = st.readMetadata;
  // Placeholder only. Degradations are known after the sorts have run.
  header.flags = 0;
  // The blob is the LAST section, so its size affects only selfSize — every
  // section offset is already fixed by the counts. Lay out with a placeholder
  // and correct selfSize once the blob has actually been written, since the
  // author spelling each record ends up carrying is not known until the
  // one-spelling-per-person pass has run.
  layoutSections(header, st.folderBytes, 0);

  HalFile stage;
  HalFile out;
  if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) return false;
  if (!Storage.openFileForWrite("LIBIDX", NEW_PATH, out)) {
    stage.close();
    return false;
  }
  serialization::BufferedFileWriter outBuffer(out, LIBRARY_IO_BUFFER_SIZE);

  // Returns false rather than spinning. A full card makes write() return 0, and
  // the old loop never advanced past it — the device simply hung mid-rebuild with
  // no message, which is worse than any error.
  bool ioFailed = false;
  // Every final-index write goes through here. A short write on a full card
  // leaves a file that still passes the header check when the header describes
  // what was intended rather than what landed.
  const auto put = [&outBuffer, &ioFailed](const void* data, const size_t len) {
    if (ioFailed) return;
    outBuffer.write(data, len);
  };
  const auto padTo = [&outBuffer, &ioFailed, &serviceUnits](const uint32_t target) {
    if (ioFailed) return;
    static const uint8_t zeros[64] = {0};
    while (outBuffer.position() < target) {
      serviceBuilder(serviceUnits);
      const uint32_t gap = target - static_cast<uint32_t>(outBuffer.position());
      const size_t want = std::min<uint32_t>(gap, sizeof(zeros));
      outBuffer.write(zeros, want);
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
    outBuffer.flush();
    stage.close();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }

  // Author order has to be known BEFORE the records are written because its
  // permutation section is emitted first.
  // The title key array is already gone before this phase. At the 4096-record
  // ceiling this checked, phase-local allocation is 57,344 bytes.
  const bool rankable = coreSortsAvailable;
  if (rankable && n > 1) {
    LOG_DBG("LIBIDX", "author sort alloc: %u bytes, heap %u, max block %u", static_cast<unsigned>(n * sizeof(SortKey)),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  auto authorSort = rankable && n > 1 ? makeUniqueNoThrow<SortKey[]>(n) : nullptr;
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
  } else if (n > 1) {
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
    LOG_INF("LIBIDX", "%u books: sort allocation unavailable; index kept in walk order", static_cast<unsigned>(n));
    stats.ranksDegraded = true;
  }

  if (ioFailed) {
    LOG_ERR("LIBIDX", "emit failed while reading the record stage");
    outBuffer.flush();
    stage.close();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }

  // Records, in title order, with both ranks and the name offset filled in.
  //
  // Name offsets follow the title order used to emit the name blob below.
  // One pair of staging buffers on the heap, reused by both emit loops. As
  // locals they were 768 bytes each, so 1.5 KB of stack inside a function running
  // on a 4 KB task — the kind of margin that survives a test library and fails on
  // someone else's. On the heap the allocation is checked; on the stack an
  // overflow is a silent corruption.
  auto staged = makeUniqueNoThrow<StagedEntry[]>(2);
  if (!staged) {
    LOG_ERR("LIBIDX", "staging buffers alloc failed");
    outBuffer.flush();
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
    // The blob holds the path hash, basename, chosen author spelling, title, and
    // source author spelling used by later rebuilds.
    // Keeping them adjacent means no second offset has to live in the record.
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
  padTo(header.nameStart);

  uint32_t blobWritten = 0;
  for (uint16_t i = 0; i < n; i++) {
    serviceBuilder(serviceUnits);
    if (!fetch(order[i], entry)) break;
    put(&entry.pathHash, sizeof(entry.pathHash));
    put(entry.name, entry.record.nameLen);

    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    if (!fetch(order[from], canonical)) break;
    put(&canonical.authorLen, 1);
    if (canonical.authorLen > 0) put(canonical.author, canonical.authorLen);
    put(&entry.titleLen, 1);
    if (entry.titleLen > 0) put(entry.title, entry.titleLen);
    put(&entry.authorLen, 1);
    if (entry.authorLen > 0) put(entry.author, entry.authorLen);
    blobWritten += blobBytesFor(entry, canonical);
  }
  header.nameLen = blobWritten;
  header.selfSize = header.nameStart + blobWritten;
  stage.close();

  // Captured HERE, at the end of the data, and not after the header rewrite
  // below: that rewrite seeks back to 0, so asking afterwards reports 64 — the
  // header's own length — and every rebuild looks truncated.
  const uint32_t written = static_cast<uint32_t>(outBuffer.position());
  if (!outBuffer.flush()) ioFailed = true;

  header.flags =
      (stats.ranksDegraded ? CLIX_FLAG_RANKS_DEGRADED : 0) | (stats.dedupDegraded ? CLIX_FLAG_DEDUP_DEGRADED : 0);

  if (!out.seekSet(0)) {
    ioFailed = true;
  } else if (out.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header))
    ioFailed = true;
  // The file is only as long as it claims if every write landed. A full card
  // fails them silently, and the result passes the header check while carrying
  // zeros — an index that looks valid and is not.
  const bool sizeMatches = written == header.selfSize;
  const bool closed = out.close();

  if (ioFailed || !sizeMatches || !closed) {
    LOG_ERR("LIBIDX", "emit incomplete (I/O %s, close %s, size %u vs %u) — keeping the old index",
            ioFailed ? "failed" : "ok", closed ? "ok" : "failed", static_cast<unsigned>(written),
            static_cast<unsigned>(header.selfSize));
    // Leave the previous index alone. A shelf that is a rebuild out of date is
    // worth incomparably more than none at all, and a full card is exactly when
    // the reader can least afford to lose it.
    if (closed) Storage.remove(NEW_PATH);
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
  // An obsolete format starts fresh; I/O, record, and allocation failures stop
  // the rebuild so the previous index remains untouched.
  std::unique_ptr<PriorEntry[]> priorList;
  uint16_t priorCount = 0;
  uint16_t nextFirstSeen = 0;
  LibraryIndexFile previous;
  if (Storage.exists(INDEX_PATH)) {
    if (previous.openForReconciliation(INDEX_PATH)) {
      nextFirstSeen = previous.header().nextFirstSeen;
      priorCount = previous.bookCount();
      priorList = makeUniqueNoThrow<PriorEntry[]>(priorCount == 0 ? 1 : priorCount);
      if (!priorList) {
        LOG_ERR("LIBIDX", "prior index array alloc failed");
        return false;
      }

      for (uint16_t i = 0; i < priorCount; i++) {
        serviceBuilder(serviceUnits);
        ClixRecord r{};
        uint64_t pathHash = 0;
        if (!previous.readRecord(i, r) || !previous.readPathHash(r, pathHash)) {
          LOG_ERR("LIBIDX", "prior index read failed at record %u", static_cast<unsigned>(i));
          return false;
        }
        priorList[i].pathHash = pathHash;
        priorList[i].fileSize = r.fileSize;
        priorList[i].firstSeen = r.firstSeen;
        priorList[i].ordinalAndMatched = i;
      }
      std::sort(priorList.get(), priorList.get() + priorCount, priorPathLess);
    } else if (previous.ioFailed()) {
      LOG_ERR("LIBIDX", "cannot read previous index; rebuild deferred");
      return false;
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
  st.previous = previous.isOpen() ? &previous : nullptr;
  st.stats = &stats;

  if (!Storage.openFileForWrite("LIBIDX", STAGE_PATH, st.stage) ||
      !Storage.openFileForWrite("LIBIDX", folderStagePath, st.folders)) {
    LOG_ERR("LIBIDX", "cannot open staging files");
    if (st.stage) st.stage.close();
    if (st.folders) st.folders.close();
    return false;
  }

  LOG_DBG("LIBIDX", "phase prepare/prior: %ums", static_cast<unsigned>(millis() - startMs));
  [[maybe_unused]] const uint32_t walkStartMs = millis();
  bool stageFlushed = false;
  {
    serialization::BufferedFileWriter stageOut(st.stage, LIBRARY_IO_BUFFER_SIZE);
    st.stageOut = &stageOut;
    walk(st, rootPath, 0);
    st.stageOut = nullptr;
    stageFlushed = stageOut.flush();
  }
  const bool stageClosed = st.stage.close();
  const bool foldersClosed = st.folders.close();
  LOG_DBG("LIBIDX", "phase walk/metadata/stage: %ums", static_cast<unsigned>(millis() - walkStartMs));

  if (st.failed || !stageFlushed || !stageClosed || !foldersClosed) {
    LOG_ERR("LIBIDX", "staging failed; keeping the previous index");
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.dedupDegraded = st.dedupDegraded;
  stats.unchanged = st.reused;
  stats.enriched = st.enriched;

  if (previous.isOpen() && st.books == priorCount && st.reused == priorCount && stats.metadataReused == priorCount &&
      st.unreadableSkipped == 0) {
    previous.close();
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    stats.walkMs = millis() - startMs;
    LOG_INF("LIBIDX", "unchanged: %u reused, %u parsed, no replacement, %ums",
            static_cast<unsigned>(stats.metadataReused), static_cast<unsigned>(stats.parsed),
            static_cast<unsigned>(stats.walkMs));
    return true;
  }

  // --- second pass: renames, then genuinely new books ----------------------
  //
  // Resolved into RAM, never by rewriting the staging file: openFileForWrite
  // opens with O_TRUNC (SDCardManager.cpp:308), so reopening the staging file to
  // patch it empties it, and every record read afterwards comes back blank.
  // Two bytes per book is a cheaper price than that failure mode.
  //
  // A book that matched no previous path is either renamed or new. Match
  // it against the leftover previous entries by SIZE alone: across a real
  // library, two different books sharing a byte-exact size is implausible, and
  // being wrong only costs one book its place in "Recently added" and one
  // re-read. A content hash would settle it properly but would read ~12 KB per
  // book on every single verification, to decide a case that arises when someone
  // renames a file.
  [[maybe_unused]] const uint32_t reconcileStartMs = millis();
  auto resolvedFirstSeen = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!resolvedFirstSeen) {
    LOG_ERR("LIBIDX", "firstSeen array alloc failed");
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  if (st.books > 0) {
    if (priorList) std::sort(priorList.get(), priorList.get() + priorCount, priorSizeLess);
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
      PriorEntry* renamed = nullptr;
      if (priorList) {
        PriorEntry* candidate =
            std::lower_bound(priorList.get(), priorList.get() + priorCount, r.fileSize,
                             [](const PriorEntry& entry, const uint32_t size) { return entry.fileSize < size; });
        while (candidate != priorList.get() + priorCount && candidate->fileSize == r.fileSize) {
          if (!priorMatched(*candidate)) {
            renamed = candidate;
            break;
          }
          ++candidate;
        }
      }
      if (renamed) {
        markPriorMatched(*renamed);
        resolvedFirstSeen[i] = renamed->firstSeen;
        stats.renamed++;
      } else {
        resolvedFirstSeen[i] = st.nextFirstSeen++;
        stats.added++;
      }
    }
    if (!read.close()) {
      LOG_ERR("LIBIDX", "firstSeen reconciliation: stage close failed");
      Storage.remove(STAGE_PATH);
      Storage.remove(folderStagePath.c_str());
      return false;
    }
    for (uint16_t q = 0; q < priorCount; q++) {
      serviceBuilder(serviceUnits);
      if (priorList && !priorMatched(priorList[q])) stats.removed++;
    }
  }
  LOG_DBG("LIBIDX", "phase reconcile: %ums", static_cast<unsigned>(millis() - reconcileStartMs));

  // --- title order -----------------------------------------------------------
  [[maybe_unused]] const uint32_t titleStartMs = millis();
  // The walk-only allocations are released before the largest temporary block.
  previous.close();
  st.previous = nullptr;
  st.prior = nullptr;
  st.nameBuf = nullptr;
  st.stagedEntry = nullptr;
  st.dedupKeys = nullptr;
  priorList.reset();
  nameBuf.reset();
  stagedEntry.reset();
  dedupKeys.reset();

  // Read the staged fold prefixes back and sort ordinals. The checked 14-byte
  // key allocation reaches 57,344 bytes at the 4,096-record format ceiling.
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

  bool coreSortsAvailable = true;
  if (st.books > 1) {
    LOG_DBG("LIBIDX", "title sort alloc: %u bytes, heap %u, max block %u",
            static_cast<unsigned>(st.books * sizeof(SortKey)), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    auto keys = makeUniqueNoThrow<SortKey[]>(st.books);
    if (keys) {
      HalFile stage;
      if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) {
        LOG_ERR("LIBIDX", "title sort: cannot reopen the record stage");
        Storage.remove(STAGE_PATH);
        Storage.remove(folderStagePath.c_str());
        return false;
      }
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
      if (!stage.close()) {
        LOG_ERR("LIBIDX", "title sort: stage close failed");
        Storage.remove(STAGE_PATH);
        Storage.remove(folderStagePath.c_str());
        return false;
      }
      delay(1);
      std::sort(keys.get(), keys.get() + st.books, sortKeyLess);
      delay(1);
      for (uint16_t i = 0; i < st.books; i++) {
        serviceBuilder(serviceUnits);
        order[i] = keys[i].ordinal;
      }
    } else {
      stats.ranksDegraded = true;
      coreSortsAvailable = false;
      LOG_ERR("LIBIDX", "sort skipped: key array alloc failed");
    }
  }
  LOG_DBG("LIBIDX", "phase title order: %ums", static_cast<unsigned>(millis() - titleStartMs));

  [[maybe_unused]] const uint32_t emitStartMs = millis();
  const bool ok =
      emitIndex(folderStagePath.c_str(), st, order.get(), resolvedFirstSeen.get(), coreSortsAvailable, stats);
  LOG_DBG("LIBIDX", "phase author/orders/emit: %ums", static_cast<unsigned>(millis() - emitStartMs));
  Storage.remove(STAGE_PATH);
  Storage.remove(folderStagePath.c_str());

  stats.walkMs = millis() - startMs;
  stats.indexReplaced = ok;
  LOG_INF("LIBIDX",
          "%s: %u books, %u folders, %u parsed, %u metadata reused, replaced %u, %u dup dropped, %u unreadable, %ums",
          ok ? "built" : "FAILED", static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
          static_cast<unsigned>(stats.parsed), static_cast<unsigned>(stats.metadataReused),
          static_cast<unsigned>(stats.indexReplaced), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped), static_cast<unsigned>(stats.walkMs));
  return ok;
}

}  // namespace library
