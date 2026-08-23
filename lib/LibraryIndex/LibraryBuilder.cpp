#include "LibraryBuilder.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include "LibraryIndexFile.h"
#include "LibraryText.h"

namespace library {
namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/library.idx";
constexpr char NEW_PATH[] = "/.crosspoint/library.new";
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

bool sortKeyLess(const SortKey& a, const SortKey& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  return a.ordinal < b.ordinal;
}

ClixFormat formatForName(const std::string& name) {
  if (FsHelpers::checkFileExtension(name, ".epub")) return CLIX_FORMAT_EPUB;
  if (FsHelpers::checkFileExtension(name, ".txt")) return CLIX_FORMAT_TXT;
  if (FsHelpers::checkFileExtension(name, ".md")) return CLIX_FORMAT_MD;
  if (FsHelpers::checkFileExtension(name, ".xtc")) return CLIX_FORMAT_XTC;
  return CLIX_FORMAT_OTHER;
}

bool isBookName(const std::string& name) {
  return FsHelpers::checkFileExtension(name, ".epub") || FsHelpers::checkFileExtension(name, ".txt") ||
         FsHelpers::checkFileExtension(name, ".md") || FsHelpers::checkFileExtension(name, ".xtc");
}

// macOS AppleDouble sidecars and hidden entries. The file browser already hides
// these (FileBrowserActivity isMacOSMetadataEntry); the shelf must agree, or a
// card written on a Mac shows every book twice.
bool isHiddenOrSidecar(const char* name) { return name[0] == '.'; }

// Join without doubling the separator. The walk starts at "/", so a naive
// concatenation yields "//SciFi/book.epub": tolerated by the filesystem, but it
// is also the string handed to Epub and ZipFile, and to the cache-path hash that
// has to match the one the reader computed when it opened the same book.
std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty() || dir == "/") return "/" + name;
  return dir + "/" + name;
}

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
  uint16_t books = 0;
  uint16_t folderId = 0;
  uint32_t folderBytes = 0;
  uint32_t nameBytes = 0;
  uint32_t totalBookBytes = 0;
  uint16_t nextFirstSeen = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  bool booksAtRoot = false;
  bool aborted = false;
  bool readMetadata = false;
  uint16_t enriched = 0;
  HalFile folders;  // folder section, staged separately then copied in
  BuildProgressFn onProgress = nullptr;
  void* progressCtx = nullptr;
  // Books the previous index knew. Empty on a first build, in which case every
  // book is new and gets a fresh firstSeen.
  PriorEntry* prior = nullptr;
  uint16_t priorCount = 0;
  uint16_t reused = 0;
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
    if (!st.prior[i].matched && st.prior[i].nameHash == nameHash && st.prior[i].size == size) return i;
  }
  return -1;
}

// parentBasename and depth are gone with the folder-as-author rule they served:
// nothing about a book's surroundings names its author any more.
void stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize, const uint16_t folderId,
                 const std::string& fullPath) {
  StagedEntry entry{};
  // The filename is a fallback for the title and nothing else: no parsing, and
  // never an author. Per review on #2885 -- no other reader parses filenames,
  // and a name pulled out of one by pattern is a guess wearing a fact's clothes.
  std::string title = stemOf(name);
  std::string author;
  bool titleFromBook = false;
  bool authorFromBook = false;
  // Which of the two metadata paths supplied the author, so the record can say so
  // rather than claiming the cache for everything.
  bool authorFromOpf = false;

  // Prefer the reader's existing cache. For an unopened book, loadMetadata()
  // reuses the same EPUB parser but stops before the manifest, so this never
  // builds spine, TOC, CSS, cover, or section caches during the library walk.
  if (st.readMetadata && FsHelpers::hasEpubExtension(name)) {
    Epub epub(fullPath, CACHE_DIR);
    Epub::MetadataSource source = Epub::MetadataSource::NONE;
    if (epub.loadMetadata(&source)) {
      if (!epub.getTitle().empty()) {
        title = epub.getTitle();
        titleFromBook = true;
      }
      if (!epub.getAuthor().empty()) {
        author = epub.getAuthor();
        authorFromBook = true;
        authorFromOpf = source == Epub::MetadataSource::PACKAGE_DOCUMENT;
      }
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
    authorFromOpf = false;
  }

  if (titleFromBook || authorFromBook) st.enriched++;

  // An absent author is a fact, not a gap to fill: the row joins the Unknown
  // group rather than borrowing a name from its surroundings.
  // Provenance separates the two metadata paths. A book that told us nothing is
  // UNKNOWN, which is now the only way to reach that value.
  ClixAuthorProvenance provenance = authorFromOpf    ? CLIX_AUTHOR_FROM_OPF
                                    : authorFromBook ? CLIX_AUTHOR_FROM_CACHE
                                                     : CLIX_AUTHOR_UNKNOWN;

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
  entry.record.foldLen = static_cast<uint8_t>(std::min(folded.size(), CLIX_FOLD_BYTES));
  entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
  entry.record.flags = makeRecordFlags(formatForName(name), provenance, titleFromBook);
  memcpy(entry.record.fold, folded.data(), entry.record.foldLen);
  memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  memcpy(entry.name, name.data(), entry.record.nameLen);

  const std::string displayAuthor = cleanPersonName(author);
  entry.authorLen = static_cast<uint8_t>(std::min(displayAuthor.size(), STAGE_AUTHOR_BYTES));
  memcpy(entry.author, displayAuthor.data(), entry.authorLen);

  st.stage.write(reinterpret_cast<const uint8_t*>(&entry), STAGE_STRIDE);
  st.nameBytes += entry.record.nameLen;
  st.totalBookBytes += fileSize;
  st.books++;
}

void walk(WalkState& st, const std::string& path, const int depth) {
  if (st.aborted || depth > LIBRARY_MAX_DEPTH || st.books >= CLIX_MAX_RECORDS) return;

  HalFile dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  dir.rewindDirectory();

  // Names already staged from THIS directory, as 4-byte hashes rather than the
  // names themselves. A damaged FAT can enumerate the same entry twice; the
  // second one would be a phantom book the user cannot open.
  //
  // Hashes because the names do not fit. Two thousand books in one flat folder —
  // the figure LibraryFormat.h cites as the case to survive — is about 360 KB of
  // std::string against a device that has under 200 KB free, and std::vector grows
  // by throwing, so the failure is abort() and a reboot loop on every rebuild
  // rather than a degraded scan. At four bytes each the same folder costs 8 KB.
  //
  // Keyed on (name hash, size) packed into 64 bits, not the hash alone. Two
  // different books colliding in 32 bits AND sharing a byte-exact size is
  // implausible where a bare hash collision is merely unlikely, and the cost of
  // being wrong is a real book silently missing from the shelf — the failure
  // hardest to notice and hardest to explain.
  std::vector<uint64_t> seen;
  seen.reserve(32);
  std::vector<std::string> subdirs;

  bool folderEmitted = false;
  uint16_t myFolderId = 0;

  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (st.aborted || st.books >= CLIX_MAX_RECORDS) {
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

    if (isDir) {
      // Bounded for the same reason `seen` holds hashes: a pathological level
      // with thousands of directories must degrade, not abort the device.
      // Skipped ones are counted so the build can say the walk was partial.
      if (subdirs.size() >= 256) {
        st.unreadableSkipped++;
        continue;
      }
      subdirs.push_back(name);
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
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      st.duplicatesDropped++;
      continue;
    }
    seen.push_back(key);

    if (!folderEmitted) {
      // Folders are emitted lazily, so only directories that actually hold a
      // book get an id and the ids stay dense.
      myFolderId = st.folderId++;
      // In range: entries whose folder path exceeds FOLDER_PATH_BYTES were
      // skipped above, so no book reaches this line with an overlong path.
      const uint8_t pathLen = static_cast<uint8_t>(path.size());
      st.folders.write(&pathLen, 1);
      st.folders.write(reinterpret_cast<const uint8_t*>(path.data()), pathLen);
      st.folderBytes += 1u + pathLen;
      folderEmitted = true;
      if (depth == 0) st.booksAtRoot = true;
    }
    stageRecord(st, name, size, myFolderId, joinPath(path, name));

    // Once per staged book, not only once per directory: this callback is the
    // one place the caller can feed the task watchdog, and with metadata enabled
    // a single book can hold stageRecord() in SD reads for a long moment. One
    // large flat folder would otherwise starve the 5 s panic timeout — and since
    // the missing index retriggers the rebuild, the failure is a reboot loop,
    // not one crash. A break, not a return: the directory handle is still open.
    if (st.onProgress != nullptr && !st.onProgress(st.books, path.c_str(), st.progressCtx)) {
      st.aborted = true;
      break;
    }
  }
  dir.close();

  // Kept for directories that stage no books, so a deep tree of empty or
  // book-less folders still feeds the watchdog between the per-book calls.
  if (!st.aborted && st.onProgress != nullptr && !st.onProgress(st.books, path.c_str(), st.progressCtx)) {
    st.aborted = true;
    return;
  }

  // Subdirectories are walked after this directory's own handle is closed:
  // SdFat on hardware allows only one open reader at a time per path, and a
  // deep tree would otherwise hold a handle per level.
  for (const std::string& sub : subdirs) {
    walk(st, joinPath(path, sub), depth + 1);
    if (st.aborted) return;
  }
}

// Assemble the final file from the two staging files and the title order.
//
// Written to a scratch path and renamed at the end, so a power cut during this
// leaves the previous index intact rather than a header claiming records that
// were never written. The header goes down twice: once as a placeholder, and
// once for real when the counts are known — the Dictionary.cpp idiom.
// The one place that says how many bytes a book occupies in the name blob.
//
// Two loops need this number — the record loop, to set each nameOff, and the blob
// loop, to write the bytes — and they used to compute it separately with a comment
// telling the next person to keep them in step. They did not stay in step: adding
// the title field in v3 updated the blob loop only, so every record after the
// first pointed short by the accumulated title lengths. Names rendered as slices
// of their neighbours, and since readPath reads the same slot, the books could not
// be opened. selfSize stayed self-consistent, so the index passed validation and
// would not have repaired itself.
//
// A shared function makes that class of bug impossible rather than fixed.
uint32_t blobBytesFor(const StagedEntry& entry, const StagedEntry& canonical) {
  return entry.record.nameLen + 1u + canonical.authorLen + 1u + entry.titleLen;
}

bool emitIndex(const char* folderStagePath, WalkState& st, const uint16_t* order, const uint16_t* resolvedFirstSeen,
               BuildStats& stats) {
  const uint16_t n = st.books;

  // newOrdinalOf[stagingIndex] = position in title order. Needed because both
  // permutation arrays index the FINAL record order, not the walk order.
  auto newOrdinalOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (!newOrdinalOf) return false;
  for (uint16_t i = 0; i < n; i++) newOrdinalOf[order[i]] = i;

  ClixHeader header{};
  memcpy(header.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  header.formatVersion = CLIX_FORMAT_VERSION;
  header.foldVersion = CLIX_FOLD_VERSION;
  header.bookCount = n;
  header.folderCount = st.folderId;
  header.nextFirstSeen = st.nextFirstSeen;
  header.totalBookBytes = st.totalBookBytes;
  // Placeholder only. The real flags are written with the final header below,
  // once the sorts have had their chance to fail: a walk that hit the record cap
  // or was aborted is NOT complete, and degradations decided further down never
  // reached this line, so an index could claim to be whole while being neither.
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

  // Returns false rather than spinning. A full card makes write() return 0, and
  // the old loop never advanced past it — the device simply hung mid-rebuild with
  // no message, which is worse than any error.
  bool writeFailed = false;
  // Every write goes through here. Checking them one by one was the alternative,
  // and the review found the ones that had been missed — a short write on a full
  // card leaves a file that still passes the header check because the header was
  // written last and describes what was intended, not what landed.
  const auto put = [&out, &writeFailed](const void* data, const size_t len) {
    if (writeFailed) return;
    if (out.write(static_cast<const uint8_t*>(data), len) != static_cast<int>(len)) writeFailed = true;
  };
  const auto padTo = [&out, &writeFailed](const uint32_t target) {
    static const uint8_t zeros[64] = {0};
    while (out.position() < target) {
      const uint32_t gap = target - static_cast<uint32_t>(out.position());
      const size_t want = std::min<uint32_t>(gap, sizeof(zeros));
      if (out.write(zeros, want) != static_cast<int>(want)) {
        writeFailed = true;
        return;
      }
    }
  };

  // Header placeholder; rewritten below once authorRank/dateRank are known.
  put(&header, sizeof(header));
  padTo(header.folderStart);

  {
    HalFile folders;
    if (Storage.openFileForRead("LIBIDX", folderStagePath, folders)) {
      uint8_t buf[256];
      // read() returns int: a -1 error must fail the emit, not wrap into a
      // huge unsigned length.
      int got = 0;
      while ((got = folders.read(buf, sizeof(buf))) > 0) put(buf, static_cast<size_t>(got));
      if (got < 0) writeFailed = true;
      folders.close();
    } else {
      // Ignoring this would publish an all-zero folder section: selfSize still
      // matches, so the index validates, and readPath() then fails for every
      // book with nothing left to trigger a self-repair.
      LOG_ERR("LIBIDX", "folder stage unreadable: %s", folderStagePath);
      writeFailed = true;
    }
  }
  padTo(header.recordStart);

  // Author order has to be known BEFORE the records are written, because each
  // record carries its own authorRank. So: read the keys, sort, invert, then
  // write. Books with no key sort last in both directions, which is why
  // knownAuthorCount is recorded rather than a second array being stored.
  // Capped like the title sort. Uncapped, the author and date arrays alone peaked
  // near 209 KB at the 4096-record ceiling — on a device with under 200 KB free,
  // which makes the cap the difference between a degraded order and no device.
  const bool rankable = n <= LIBRARY_MAX_SORTED;
  auto authorSort = rankable ? makeUniqueNoThrow<SortKey[]>(n == 0 ? 1 : n) : nullptr;
  auto authorRankOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  // The pair degrades together. Consumers test one pointer each, so a key
  // array without its rank array would feed zeroed keys to the spelling vote
  // and zeroed ordinals to the permutation section.
  if (!authorSort || !authorRankOf) {
    authorSort.reset();
    authorRankOf.reset();
  }
  uint16_t known = 0;
  if (authorSort && authorRankOf) {
    for (uint16_t i = 0; i < n; i++) {
      ClixRecord r{};
      stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
      stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
      if (r.authorKeyLen == 0) {
        // 0xFF outranks every folded byte, so unknown authors land at the end.
        memset(authorSort[i].key, 0xFF, sizeof(authorSort[i].key));
      } else {
        memset(authorSort[i].key, 0, sizeof(authorSort[i].key));
        memcpy(authorSort[i].key, r.authorKey, std::min<size_t>(r.authorKeyLen, sizeof(authorSort[i].key)));
        known++;
      }
      authorSort[i].ordinal = i;
    }
    if (n > 1) std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
    for (uint16_t k = 0; k < n; k++) authorRankOf[authorSort[k].ordinal] = k;
  } else {
    for (uint16_t i = 0; i < n; i++) {
      if (authorRankOf) authorRankOf[i] = i;
    }
    stats.ranksDegraded = true;
  }
  header.knownAuthorCount = known;

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
  auto canonicalFrom = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (canonicalFrom) {
    for (uint16_t i = 0; i < n; i++) canonicalFrom[i] = i;
  }
  if (canonicalFrom && authorSort && n > 1) {
    uint16_t runStart = 0;
    while (runStart < n) {
      uint16_t runEnd = runStart + 1;
      while (runEnd < n &&
             memcmp(authorSort[runEnd].key, authorSort[runStart].key, sizeof(authorSort[runStart].key)) == 0) {
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
        struct Spelling {
          std::string text;
          uint16_t ordinal;
          int count;
        };
        std::vector<Spelling> spellings;
        spellings.reserve(4);

        for (uint16_t a = runStart; a < runEnd; a++) {
          const uint16_t ord = authorSort[a].ordinal;
          uint8_t len = 0;
          stage.seekSet(static_cast<uint64_t>(order[ord]) * STAGE_STRIDE + offsetof(StagedEntry, authorLen));
          if (stage.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len) || len == 0) continue;

          char buf[STAGE_AUTHOR_BYTES];
          const size_t want = std::min<size_t>(len, sizeof(buf));
          stage.seekSet(static_cast<uint64_t>(order[ord]) * STAGE_STRIDE + offsetof(StagedEntry, author));
          if (stage.read(reinterpret_cast<uint8_t*>(buf), want) != static_cast<int>(want)) continue;
          const std::string text(buf, want);

          bool merged = false;
          for (auto& sp : spellings) {
            if (sp.text == text) {
              sp.count++;
              merged = true;
              break;
            }
          }
          // A hard cap so a card full of near-identical spellings cannot grow this
          // without bound. Sixteen is far past anything real; beyond it the vote
          // simply decides among the first sixteen.
          if (!merged && spellings.size() < 16) spellings.push_back(Spelling{text, ord, 1});
        }

        uint16_t bestOrdinal = authorSort[runStart].ordinal;
        int bestScore = -1;
        size_t bestLen = 0;
        std::string bestText;
        for (const auto& sp : spellings) {
          const bool better = sp.count > bestScore || (sp.count == bestScore && sp.text.size() < bestLen) ||
                              (sp.count == bestScore && sp.text.size() == bestLen && sp.text < bestText);
          if (better) {
            bestScore = sp.count;
            bestLen = sp.text.size();
            bestText = sp.text;
            bestOrdinal = sp.ordinal;
          }
        }
        for (uint16_t a = runStart; a < runEnd; a++) canonicalFrom[authorSort[a].ordinal] = bestOrdinal;
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
  if (authorSort && authorRankOf && canonicalFrom && n > 1) {
    for (uint16_t i = 0; i < n; i++) {
      // canonicalFrom holds TITLE-order positions, and the staging file is keyed
      // by walk order — order[] is the map between them. Reading staging with the
      // title position directly fetches an unrelated book, which is what split
      // John Scalzi into two groups and left the shelf in no order at all.
      const uint16_t src = order[canonicalFrom[i]];
      uint8_t authorLen = 0;
      char author[STAGE_AUTHOR_BYTES] = {};
      stage.seekSet(static_cast<uint64_t>(src) * STAGE_STRIDE + offsetof(StagedEntry, authorLen));
      stage.read(reinterpret_cast<uint8_t*>(&authorLen), sizeof(authorLen));
      if (authorLen > 0) {
        stage.seekSet(static_cast<uint64_t>(src) * STAGE_STRIDE + offsetof(StagedEntry, author));
        stage.read(reinterpret_cast<uint8_t*>(author), std::min<size_t>(authorLen, sizeof(author)));
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
    std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
    for (uint16_t k = 0; k < n; k++) authorRankOf[authorSort[k].ordinal] = k;
  }

  // --- arrival order -------------------------------------------------------
  //
  // firstSeen values now come from the PREVIOUS index, so they are no longer a
  // dense sequence in walk order: a rebuild reuses each book's original number
  // and only hands out new ones for books it has never seen. The date order has
  // to be SORTED rather than assumed, or "Recently added" silently degrades into
  // "the order the card enumerates in" — which is exactly the bug reconciliation
  // exists to prevent.
  auto dateSort = rankable ? makeUniqueNoThrow<SortKey[]>(n == 0 ? 1 : n) : nullptr;
  // Same pairing rule as the author arrays above.
  // Said out loud, so the shelf can tell the reader its order is approximate
  // rather than quietly presenting walk order as an alphabet.
  if (!rankable) {
    LOG_INF("LIBIDX", "%u books over the %u sort cap: author and date order degraded", static_cast<unsigned>(n),
            static_cast<unsigned>(LIBRARY_MAX_SORTED));
    stats.ranksDegraded = true;
  }
  auto dateRankOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (dateSort && dateRankOf) {
    for (uint16_t i = 0; i < n; i++) {
      ClixRecord r{};
      stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
      stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
      // Big-endian into the key so memcmp orders numerically.
      const uint16_t seen = resolvedFirstSeen ? resolvedFirstSeen[order[i]] : r.firstSeen;
      memset(dateSort[i].key, 0, sizeof(dateSort[i].key));
      dateSort[i].key[0] = static_cast<char>(seen >> 8);
      dateSort[i].key[1] = static_cast<char>(seen & 0xFF);
      dateSort[i].ordinal = i;
    }
    if (n > 1) std::sort(dateSort.get(), dateSort.get() + n, sortKeyLess);
    for (uint16_t k = 0; k < n; k++) dateRankOf[dateSort[k].ordinal] = k;
  } else {
    stats.ranksDegraded = true;
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
  const auto fetch = [&stage, &writeFailed](const uint16_t stagingIndex, StagedEntry& dest) {
    stage.seekSet(static_cast<uint64_t>(stagingIndex) * STAGE_STRIDE);
    if (stage.read(reinterpret_cast<uint8_t*>(&dest), STAGE_STRIDE) != static_cast<int>(STAGE_STRIDE)) {
      writeFailed = true;
      return false;
    }
    return true;
  };

  uint32_t nameCursor = 0;
  for (uint16_t i = 0; i < n; i++) {
    if (!fetch(order[i], entry)) break;
    entry.record.nameOff = nameCursor;
    // The blob holds the basename, then one length byte, then the chosen author
    // spelling, then the title. Keeping them adjacent means no second offset has
    // to live in the record, which is exactly full at 128 bytes.
    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    if (!fetch(order[from], canonical)) break;
    nameCursor += blobBytesFor(entry, canonical);
    if (resolvedFirstSeen) entry.record.firstSeen = resolvedFirstSeen[order[i]];
    entry.record.dateRank = dateRankOf ? dateRankOf[i] : i;
    entry.record.authorRank = authorRankOf ? authorRankOf[i] : i;
    put(&entry.record, sizeof(ClixRecord));
  }
  padTo(header.permStart);

  for (uint16_t k = 0; k < n; k++) {
    const uint16_t ordinal = authorSort ? authorSort[k].ordinal : k;
    put(&ordinal, sizeof(ordinal));
  }
  for (uint16_t k = 0; k < n; k++) {
    const uint16_t ordinal = dateSort ? dateSort[k].ordinal : newOrdinalOf[k];
    put(&ordinal, sizeof(ordinal));
  }
  padTo(header.nameStart);

  uint32_t blobWritten = 0;
  for (uint16_t i = 0; i < n; i++) {
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

  // Now that every sort has run, say what this index actually is. A walk stopped
  // by the record cap or by an abort is not complete, and a reader that trusts
  // WALK_COMPLETE would silently show a partial shelf as if it were the whole one.
  header.flags = (st.aborted || st.books >= CLIX_MAX_RECORDS ? 0 : CLIX_FLAG_WALK_COMPLETE) |
                 (stats.ranksDegraded ? CLIX_FLAG_RANKS_DEGRADED : 0) |
                 (stats.booksAtRoot ? CLIX_FLAG_BOOKS_AT_ROOT : 0);

  out.seekSet(0);
  put(&header, sizeof(header));
  // The file is only as long as it claims if every write landed. A full card
  // fails them silently, and the result passes the header check while carrying
  // zeros — an index that looks valid and is not.
  const bool sizeMatches = written == header.selfSize;
  out.close();

  if (writeFailed || !sizeMatches) {
    LOG_ERR("LIBIDX", "emit incomplete (write %s, size %u vs %u) — keeping the old index",
            writeFailed ? "failed" : "ok", static_cast<unsigned>(written), static_cast<unsigned>(header.selfSize));
    // Leave the previous index alone. A shelf that is a rebuild out of date is
    // worth incomparably more than none at all, and a full card is exactly when
    // the reader can least afford to lose it.
    Storage.remove(NEW_PATH);
    return false;
  }

  // Rename last. Until this line the previous index is still the live one.
  Storage.remove(INDEX_PATH);
  if (!Storage.rename(NEW_PATH, INDEX_PATH)) {
    LOG_ERR("LIBIDX", "rename %s -> %s failed", NEW_PATH, INDEX_PATH);
    Storage.remove(NEW_PATH);
    return false;
  }
  return true;
}

}  // namespace

const char* libraryIndexPath() { return INDEX_PATH; }
const char* libraryStagePath() { return STAGE_PATH; }

bool buildLibraryIndex(const char* rootPath, const uint16_t previousNextFirstSeen, BuildStats& stats,
                       const bool readMetadata, const BuildProgressFn onProgress, void* progressCtx) {
  const uint32_t startMs = millis();
  stats = BuildStats{};

  Storage.mkdir(CACHE_DIR);
  Storage.remove(STAGE_PATH);
  const std::string folderStagePath = std::string(STAGE_PATH) + ".f";
  Storage.remove(folderStagePath.c_str());

  auto nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuf) {
    LOG_ERR("LIBIDX", "name buffer alloc failed (%u bytes)", static_cast<unsigned>(NAME_BUF_SIZE));
    return false;
  }

  // Load what the previous index knew, so the walk can recognise the same books.
  // Failure here is not fatal: the build simply treats every book as new.
  std::unique_ptr<PriorEntry[]> priorList;
  uint16_t priorCount = 0;
  {
    LibraryIndexFile previous;
    if (previous.open(INDEX_PATH)) {
      priorCount = previous.bookCount();
      priorList = makeUniqueNoThrow<PriorEntry[]>(priorCount == 0 ? 1 : priorCount);
      if (priorList) {
        uint16_t kept = 0;
        for (uint16_t i = 0; i < priorCount; i++) {
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
  st.nextFirstSeen = previousNextFirstSeen;
  st.prior = priorList.get();
  st.priorCount = priorList ? priorCount : 0;
  st.readMetadata = readMetadata;
  st.onProgress = onProgress;
  st.progressCtx = progressCtx;

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

  // An aborted walk must leave the previous index in place: the staging file
  // is partial, and every section derived from it would inherit the holes.
  if (st.aborted) {
    LOG_INF("LIBIDX", "walk aborted; keeping the previous index");
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
  if (!st.aborted && st.books > 0) {
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
      ClixRecord r{};
      read.seekSet(static_cast<uint64_t>(i) * STAGE_STRIDE);
      if (read.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) {
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
      if (priorList && !priorList[q].matched) stats.removed++;
    }
  }

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.booksAtRoot = st.booksAtRoot;
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
  for (uint16_t i = 0; i < st.books; i++) order[i] = i;

  if (sortable && st.books > 1) {
    auto keys = makeUniqueNoThrow<SortKey[]>(st.books);
    HalFile stage;
    if (keys && Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) {
      for (uint16_t i = 0; i < st.books; i++) {
        ClixRecord r{};
        stage.seekSet(static_cast<uint64_t>(i) * STAGE_STRIDE);
        stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
        memset(keys[i].key, 0, sizeof(keys[i].key));
        memcpy(keys[i].key, r.fold, std::min<size_t>(r.foldLen, sizeof(keys[i].key)));
        keys[i].ordinal = i;
      }
      stage.close();
      std::sort(keys.get(), keys.get() + st.books, sortKeyLess);
      for (uint16_t i = 0; i < st.books; i++) order[i] = keys[i].ordinal;
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
  LOG_INF("LIBIDX", "%s: %u books, %u folders, %u dup dropped, %u unreadable, %ums", ok ? "built" : "FAILED",
          static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
          static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped),
          static_cast<unsigned>(stats.walkMs));
  return ok;
}

}  // namespace library
