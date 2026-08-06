#pragma once

// On-disk layout of the Library index (`CLX1`), plus the pure validation and
// offset arithmetic around it.
//
// Deliberately free of HalStorage and Arduino so the whole layout is
// host-testable (test/library_format). File I/O lives in LibraryIndexFile.
//
// Layout, in order, every section starting on a 512-byte boundary:
//
//   header        64 bytes of struct, padded to 512
//   folders       F variable-length records; the id of a folder IS its ordinal
//   records       N x exactly 128 bytes, in folded-title order
//   permutations  authorOrder[N] then dateOrder[N], both u16
//   names         raw display basenames, no NULs, lengths held in the records
//
// The fixed 128-byte record stride is the load-bearing choice: record k lives at
// recordStart + 128k, so paging is O(1) in every sort order with no offset
// table; 32 records fill a 4096-byte buffer exactly, so a streaming scan never
// straddles a record; and since recordStart is 512-aligned and 128 divides 512,
// every chunk read is aligned by construction rather than by remembering to.

#include <cstddef>
#include <cstdint>

namespace library {

inline constexpr char CLIX_MAGIC[4] = {'C', 'L', 'X', '1'};
// 2: author order became surname-first.
// 3: the name blob carries the book's own title after the author, and `name` went
//    back to being the FILENAME — writing the title there broke opening a book,
//    since the file path is rebuilt from that slot.
// Bumping this is the whole migration: an index from an older version fails
// validation and is rebuilt.
inline constexpr uint8_t CLIX_FORMAT_VERSION = 3;

// Bump when the fold or the article table changes. Forces fold and ranks to be
// rebuilt while firstSeen values are preserved, so "recently added" survives.
inline constexpr uint8_t CLIX_FOLD_VERSION = 1;

inline constexpr uint32_t CLIX_ALIGN = 512;
inline constexpr size_t CLIX_FOLD_BYTES = 96;
inline constexpr size_t CLIX_AUTHOR_KEY_BYTES = 12;

// A 2000-book card already produces a 429 KiB index; beyond this the design's
// own measurements stop holding, so the walk stops and flags the index partial
// rather than silently producing something it cannot page.
inline constexpr uint16_t CLIX_MAX_RECORDS = 4096;

enum ClixFlags : uint8_t {
  CLIX_FLAG_WALK_COMPLETE = 1 << 0,
  CLIX_FLAG_RANKS_DEGRADED = 1 << 1,
  CLIX_FLAG_ENRICH_COMPLETE = 1 << 2,
  CLIX_FLAG_BOOKS_AT_ROOT = 1 << 3,
};

enum ClixFormat : uint8_t {
  CLIX_FORMAT_EPUB = 0,
  CLIX_FORMAT_TXT = 1,
  CLIX_FORMAT_MD = 2,
  CLIX_FORMAT_XTC = 3,
  CLIX_FORMAT_OTHER = 4,
};

// Where a record's author came from. Surfaced in the Details row so the screen
// never silently claims a folder name is an author.
enum ClixAuthorProvenance : uint8_t {
  // Legacy. A parent folder used to become the author when nothing else
  // named one; that rule is gone (no reader parses its way to an author), and
  // nothing writes this any more. The value stays so indexes written by an
  // older build still decode.
  CLIX_AUTHOR_FROM_FOLDER = 0,
  CLIX_AUTHOR_FROM_CACHE = 1,
  CLIX_AUTHOR_FROM_OPF = 2,
  CLIX_AUTHOR_UNKNOWN = 3,
};

#pragma pack(push, 1)

struct ClixHeader {
  char magic[4];
  uint8_t formatVersion;
  uint8_t foldVersion;
  uint8_t flags;
  uint8_t reserved0;
  uint16_t bookCount;
  uint16_t folderCount;
  // Books with a resolvable author key. Author-descending reverses only this
  // leading block, so unknown authors stay last in both directions without
  // needing a second permutation array.
  uint16_t knownAuthorCount;
  uint16_t nextFirstSeen;
  // Reserved. These held enrichCursor and longestName[3], added for an
  // incremental-enrichment pass and a layout hint that were never built — nothing
  // has ever written or read them. Named fields that no code maintains are worse
  // than blank space: the next reader assumes they mean something. Kept as bytes
  // so the header stays 64 and the format version does not have to move.
  uint8_t reserved1[8];
  uint32_t folderStart;
  uint32_t folderLen;
  uint32_t recordStart;
  uint32_t permStart;
  uint32_t nameStart;
  uint32_t nameLen;
  // Expected total file size. Comparing it with the real size is a free
  // truncation guard: a build interrupted by a power cut cannot pass.
  uint32_t selfSize;
  // Reserved; was scanSignature, intended to detect a card changing under the
  // index and never wired to anything.
  uint32_t reserved2;
  uint32_t lastWalkMs;
  uint32_t totalBookBytes;
};
static_assert(sizeof(ClixHeader) == 64, "ClixHeader must be exactly 64 bytes");

struct ClixRecord {
  uint32_t nameOff;   // from nameStart, into the display-name blob
  uint32_t fileSize;  // captured while the dirent was open; part of the identity
  uint16_t authorRank;
  uint16_t dateRank;
  uint16_t firstSeen;
  uint16_t folderId;
  uint8_t nameLen;
  uint8_t foldLen;
  uint8_t authorKeyLen;
  uint8_t flags;  // b0-2 format, b3-4 author provenance, b5 titleFromOpf, b6 opfTooLarge
  char fold[CLIX_FOLD_BYTES];
  char authorKey[CLIX_AUTHOR_KEY_BYTES];
};
static_assert(sizeof(ClixRecord) == 128, "ClixRecord must be exactly 128 bytes");
static_assert(CLIX_ALIGN % sizeof(ClixRecord) == 0, "records must tile a 512-byte sector");

struct ClixFolderHeader {
  uint8_t pathLen;  // 1..255; the path bytes follow, with no trailing '/'
};
static_assert(sizeof(ClixFolderHeader) == 1, "ClixFolderHeader must be 1 byte");

#pragma pack(pop)

// Record field accessors. The packing means the flags byte carries three things,
// so nobody outside this header should be shifting bits by hand.
inline ClixFormat recordFormat(const ClixRecord& r) { return static_cast<ClixFormat>(r.flags & 0x07); }
inline ClixAuthorProvenance recordAuthorProvenance(const ClixRecord& r) {
  return static_cast<ClixAuthorProvenance>((r.flags >> 3) & 0x03);
}
inline bool recordTitleFromOpf(const ClixRecord& r) { return (r.flags & (1 << 5)) != 0; }
inline bool recordOpfTooLarge(const ClixRecord& r) { return (r.flags & (1 << 6)) != 0; }
inline uint8_t makeRecordFlags(const ClixFormat format, const ClixAuthorProvenance provenance, const bool titleFromOpf,
                               const bool opfTooLarge) {
  return static_cast<uint8_t>((format & 0x07) | ((provenance & 0x03) << 3) | (titleFromOpf ? (1 << 5) : 0) |
                              (opfTooLarge ? (1 << 6) : 0));
}

inline uint32_t alignUp(const uint32_t value) { return (value + CLIX_ALIGN - 1) / CLIX_ALIGN * CLIX_ALIGN; }

// Fill in every offset and the expected file size from the counts alone, so the
// writer and the reader can never disagree about where a section starts.
inline void layoutSections(ClixHeader& h, const uint32_t folderBytes, const uint32_t nameBytes) {
  h.folderStart = CLIX_ALIGN;
  h.folderLen = folderBytes;
  h.recordStart = alignUp(h.folderStart + folderBytes);
  h.permStart = alignUp(h.recordStart + static_cast<uint32_t>(h.bookCount) * sizeof(ClixRecord));
  h.nameStart = alignUp(h.permStart + static_cast<uint32_t>(h.bookCount) * 2u * sizeof(uint16_t));
  h.nameLen = nameBytes;
  h.selfSize = h.nameStart + nameBytes;
}

inline uint32_t recordOffset(const ClixHeader& h, const uint16_t ordinal) {
  return h.recordStart + static_cast<uint32_t>(ordinal) * sizeof(ClixRecord);
}
inline uint32_t authorOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + static_cast<uint32_t>(k) * sizeof(uint16_t);
}
inline uint32_t dateOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + (static_cast<uint32_t>(h.bookCount) + k) * sizeof(uint16_t);
}

// Why a loaded index was rejected. Reported rather than swallowed so a rebuild
// loop caused by a format bug shows up in the log instead of looking like a slow
// first boot.
enum class ClixValidity : uint8_t {
  Ok,
  BadMagic,
  UnknownFormatVersion,
  StaleFoldVersion,
  SizeMismatch,  // truncated, or a build interrupted before the final rename
  CountOutOfRange,
  SectionsInconsistent,
};

// Validate a header against the real file size. Cheap enough to run on the one
// sector already read, and strict enough that nothing downstream has to
// re-check bounds.
inline ClixValidity validateHeader(const ClixHeader& h, const uint64_t actualFileSize) {
  for (size_t i = 0; i < sizeof(CLIX_MAGIC); i++) {
    if (h.magic[i] != CLIX_MAGIC[i]) return ClixValidity::BadMagic;
  }
  if (h.formatVersion != CLIX_FORMAT_VERSION) return ClixValidity::UnknownFormatVersion;
  if (h.foldVersion != CLIX_FOLD_VERSION) return ClixValidity::StaleFoldVersion;
  if (h.bookCount > CLIX_MAX_RECORDS) return ClixValidity::CountOutOfRange;
  if (actualFileSize != h.selfSize) return ClixValidity::SizeMismatch;

  // Both lengths are attacker-controlled bytes. Capped against the real file
  // size they cannot wrap the 32-bit section sums below, so the layout
  // comparison stays sound instead of re-deriving the same wrapped values.
  if (h.folderLen > actualFileSize || h.nameLen > actualFileSize) return ClixValidity::SectionsInconsistent;

  ClixHeader expected = h;
  layoutSections(expected, h.folderLen, h.nameLen);
  if (expected.folderStart != h.folderStart || expected.recordStart != h.recordStart ||
      expected.permStart != h.permStart || expected.nameStart != h.nameStart || expected.selfSize != h.selfSize) {
    return ClixValidity::SectionsInconsistent;
  }
  if (h.knownAuthorCount > h.bookCount) return ClixValidity::SectionsInconsistent;
  return ClixValidity::Ok;
}

const char* clixValidityName(ClixValidity v);

}  // namespace library
