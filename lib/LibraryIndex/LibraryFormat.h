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
//   permutations  authorOrder[N], arrivalOrder[N], seriesOrder[N], all u16
//   series        S x exactly 64 bytes; the id of a series IS its ordinal
//   seriesRef     N x exactly 4 bytes, parallel to the records
//   names         raw display basenames, no NULs, lengths held in the records
//
// The fixed 128-byte record stride is the load-bearing choice: record k lives at
// recordStart + 128k, so paging is O(1) in every sort order with no offset
// table; 32 records fill a 4096-byte buffer exactly, so a streaming scan never
// straddles a record; and since recordStart is 512-aligned and 128 divides 512,
// every chunk read is aligned by construction rather than by remembering to.
//
// The series sections repeat that choice rather than inventing a second one. A
// variable-length series table like the folder table would cost a walk from the
// start of the section on every heading drawn — folders get away with it because
// a row reads one, but a shelf in series order reads a heading per group. At a
// fixed 64 bytes, series s is at seriesStart + 64s and 8 of them tile a sector.

#include <cstddef>
#include <cstdint>

namespace library {

inline constexpr char CLIX_MAGIC[4] = {'C', 'L', 'X', '1'};
// 2: series. A third permutation, a fixed-stride series table and a per-book
//    reference, all in sections of their own — the record is exactly full at 128
//    bytes and widening it would cost the properties that stride buys.
// Bumping this is the whole migration: an index from an older version fails
// validation and is rebuilt.
inline constexpr uint8_t CLIX_FORMAT_VERSION = 2;

// Bump when the fold or the article table changes. Forces fold and ranks to be
// rebuilt while firstSeen values are preserved, so "recently added" survives.
inline constexpr uint8_t CLIX_FOLD_VERSION = 2;

inline constexpr uint32_t CLIX_ALIGN = 512;
inline constexpr size_t CLIX_FOLD_BYTES = 96;
inline constexpr size_t CLIX_AUTHOR_KEY_BYTES = 12;

// A 2000-book card already produces a 429 KiB index. This hard bound keeps every
// record count and permutation ordinal representable by uint16_t.
inline constexpr uint16_t CLIX_MAX_RECORDS = 4096;

// A book that belongs to no series. Not 0, which is a real series id.
inline constexpr uint16_t CLIX_SERIES_NONE = 0xFFFF;

// A book's position within its series, held as hundredths so that the
// fractional numbering Calibre uses for novellas and interstitials — 0.5 for a
// prequel, 3.5 for a between-books novella — sorts where it belongs instead of
// colliding with a neighbour. 250 is "2.5".
//
// Absence needs its own value rather than 0, because 0 is a real position: it is
// what a prequel published before book one is usually given.
inline constexpr uint16_t SERIES_INDEX_NONE = 0xFFFF;
// Highest representable position, 655.34. Positions beyond it are clamped.
inline constexpr uint16_t SERIES_INDEX_MAX = 0xFFFE;
// Series entries are a fixed stride for the same reason records are: the shelf
// draws a heading per group, and a variable-length table would make that a walk.
inline constexpr size_t CLIX_SERIES_ENTRY_BYTES = 64;
inline constexpr size_t CLIX_SERIES_NAME_BYTES = CLIX_SERIES_ENTRY_BYTES - 3;

enum ClixFlags : uint8_t {
  CLIX_FLAG_RANKS_DEGRADED = 1 << 0,
  CLIX_FLAG_DEDUP_DEGRADED = 1 << 1,
  // The walk opened each book's package document for its title, author and
  // series. Recorded because it decides what the index CAN hold — with it clear
  // there are no series at all — so a reader can tell a card whose books name no
  // series from an index built before the setting was turned on, and rebuild
  // rather than show an empty shelf.
  CLIX_FLAG_USED_METADATA = 1 << 2,
};

#pragma pack(push, 1)

struct ClixHeader {
  char magic[4];
  uint8_t formatVersion;
  uint8_t foldVersion;
  uint8_t flags;
  uint8_t padding0;
  uint16_t bookCount;
  uint16_t folderCount;
  uint16_t nextFirstSeen;
  // Distinct series across the library.
  uint16_t seriesCount;
  uint32_t folderStart;
  uint32_t folderLen;
  uint32_t recordStart;
  uint32_t permStart;
  uint32_t seriesStart;
  uint32_t seriesRefStart;
  uint32_t nameStart;
  uint32_t nameLen;
  // Expected total file size. Comparing it with the real size is a free
  // truncation guard: a build interrupted by a power cut cannot pass.
  uint32_t selfSize;
  // Books belonging to a series. Series order puts the grouped books first, so
  // this is where the standalones begin.
  uint16_t knownSeriesCount;
  uint8_t reserved[10];
};
static_assert(sizeof(ClixHeader) == 64, "ClixHeader must be exactly 64 bytes");

struct ClixRecord {
  uint32_t nameOff;   // from nameStart, into the display-name blob
  uint32_t fileSize;  // captured while the dirent was open; part of the identity
  uint16_t firstSeen;
  uint16_t folderId;
  uint8_t nameLen;
  uint8_t foldLen;
  uint8_t authorKeyLen;
  uint8_t padding;
  char fold[CLIX_FOLD_BYTES];
  char authorKey[CLIX_AUTHOR_KEY_BYTES];
  uint8_t reserved[4];
};
static_assert(sizeof(ClixRecord) == 128, "ClixRecord must be exactly 128 bytes");
static_assert(CLIX_ALIGN % sizeof(ClixRecord) == 0, "records must tile a 512-byte sector");

struct ClixFolderHeader {
  uint8_t pathLen;  // 1..255; the path bytes follow, with no trailing '/'
};
static_assert(sizeof(ClixFolderHeader) == 1, "ClixFolderHeader must be 1 byte");

// One series, at seriesStart + 64 * id.
//
// bookCount is stored rather than counted because the heading wants it and the
// group is only contiguous in ONE of the sort orders; deriving it would mean a
// scan of the permutation on every draw.
struct ClixSeriesEntry {
  uint16_t bookCount;
  // <= CLIX_SERIES_NAME_BYTES. Longer names are truncated at the last complete
  // UTF-8 codepoint, since this string is drawn as the group heading.
  uint8_t nameLen;
  char name[CLIX_SERIES_NAME_BYTES];
};
static_assert(sizeof(ClixSeriesEntry) == CLIX_SERIES_ENTRY_BYTES, "ClixSeriesEntry must be exactly 64 bytes");
static_assert(CLIX_ALIGN % sizeof(ClixSeriesEntry) == 0, "series entries must tile a 512-byte sector");

// A book's place in its series, at seriesRefStart + 4 * ordinal — parallel to the
// records, so a record and its reference are found the same way.
//
// Separate from ClixRecord because that struct is exactly full at 128 bytes and
// the stride is what makes paging O(1); widening it to 132 would break the
// tiling that the whole streaming design rests on.
struct ClixSeriesRef {
  uint16_t seriesId;  // CLIX_SERIES_NONE for a standalone book
  // Position within the series in hundredths, so 2.5 sorts between 2 and 3.
  // SERIES_INDEX_NONE when the book names a series but no position.
  uint16_t seriesIndex;
};
static_assert(sizeof(ClixSeriesRef) == 4, "ClixSeriesRef must be exactly 4 bytes");

#pragma pack(pop)

inline uint32_t alignUp(const uint32_t value) { return (value + CLIX_ALIGN - 1) / CLIX_ALIGN * CLIX_ALIGN; }

// Fill in every offset and the expected file size from the counts alone, so the
// writer and the reader can never disagree about where a section starts.
//
// Reads h.bookCount and h.seriesCount, so both must be set before calling.
inline void layoutSections(ClixHeader& h, const uint32_t folderBytes, const uint32_t nameBytes) {
  const uint32_t books = h.bookCount;
  h.folderStart = CLIX_ALIGN;
  h.folderLen = folderBytes;
  h.recordStart = alignUp(h.folderStart + folderBytes);
  h.permStart = alignUp(h.recordStart + books * sizeof(ClixRecord));
  h.seriesStart = alignUp(h.permStart + books * 3u * sizeof(uint16_t));
  h.seriesRefStart = alignUp(h.seriesStart + static_cast<uint32_t>(h.seriesCount) * sizeof(ClixSeriesEntry));
  h.nameStart = alignUp(h.seriesRefStart + books * sizeof(ClixSeriesRef));
  h.nameLen = nameBytes;
  h.selfSize = h.nameStart + nameBytes;
}

inline uint32_t recordOffset(const ClixHeader& h, const uint16_t ordinal) {
  return h.recordStart + static_cast<uint32_t>(ordinal) * sizeof(ClixRecord);
}
inline uint32_t authorOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + static_cast<uint32_t>(k) * sizeof(uint16_t);
}
inline uint32_t arrivalOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + (static_cast<uint32_t>(h.bookCount) + k) * sizeof(uint16_t);
}
inline uint32_t seriesOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + (2u * static_cast<uint32_t>(h.bookCount) + k) * sizeof(uint16_t);
}
inline uint32_t seriesEntryOffset(const ClixHeader& h, const uint16_t seriesId) {
  return h.seriesStart + static_cast<uint32_t>(seriesId) * sizeof(ClixSeriesEntry);
}
inline uint32_t seriesRefOffset(const ClixHeader& h, const uint16_t ordinal) {
  return h.seriesRefStart + static_cast<uint32_t>(ordinal) * sizeof(ClixSeriesRef);
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
inline ClixValidity validateHeaderStructure(const ClixHeader& h, const uint64_t actualFileSize) {
  for (size_t i = 0; i < sizeof(CLIX_MAGIC); i++) {
    if (h.magic[i] != CLIX_MAGIC[i]) return ClixValidity::BadMagic;
  }
  if (h.formatVersion != CLIX_FORMAT_VERSION) return ClixValidity::UnknownFormatVersion;
  if (h.bookCount > CLIX_MAX_RECORDS) return ClixValidity::CountOutOfRange;
  if (actualFileSize != h.selfSize) return ClixValidity::SizeMismatch;

  // Both lengths are attacker-controlled bytes. Capped against the real file
  // size they cannot wrap the 32-bit section sums below, so the layout
  // comparison stays sound instead of re-deriving the same wrapped values.
  if (h.folderLen > actualFileSize || h.nameLen > actualFileSize) return ClixValidity::SectionsInconsistent;

  // A book can belong to at most one series, so more series than books is
  // arithmetically impossible and the layout below would be reading a count the
  // writer never produced.
  if (h.seriesCount > h.bookCount) return ClixValidity::CountOutOfRange;
  if (h.knownSeriesCount > h.bookCount) return ClixValidity::CountOutOfRange;
  // With no series there can be no book in one.
  if (h.seriesCount == 0 && h.knownSeriesCount != 0) return ClixValidity::CountOutOfRange;
  // And no series is empty — every group is counted from the books in it — so
  // more series than grouped books is a shape the writer cannot produce.
  if (h.seriesCount > h.knownSeriesCount) return ClixValidity::CountOutOfRange;

  ClixHeader expected = h;
  layoutSections(expected, h.folderLen, h.nameLen);
  if (expected.folderStart != h.folderStart || expected.recordStart != h.recordStart ||
      expected.permStart != h.permStart || expected.seriesStart != h.seriesStart ||
      expected.seriesRefStart != h.seriesRefStart || expected.nameStart != h.nameStart ||
      expected.selfSize != h.selfSize) {
    return ClixValidity::SectionsInconsistent;
  }
  return ClixValidity::Ok;
}

inline ClixValidity validateHeader(const ClixHeader& h, const uint64_t actualFileSize) {
  const ClixValidity structure = validateHeaderStructure(h, actualFileSize);
  if (structure != ClixValidity::Ok) return structure;
  return h.foldVersion == CLIX_FOLD_VERSION ? ClixValidity::Ok : ClixValidity::StaleFoldVersion;
}

const char* clixValidityName(ClixValidity v);

}  // namespace library
