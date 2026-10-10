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
//   permutations  authorOrder[N], arrivalOrder[N], then groupOrder[N], all u16
//   groups        G x exactly 64 bytes; the group id is its ordinal
//   groupRefs     N x 4 bytes, parallel to records
//   names         path hash, filename, display author, title, source author,
//                 series position, and source group field blobs
//
// The fixed 128-byte record stride is the load-bearing choice: record k lives at
// recordStart + 128k, so paging is O(1) in every sort order with no offset
// table; 32 records fill a 4096-byte buffer exactly, so a streaming scan never
// straddles a record; and since recordStart is 512-aligned and 128 divides 512,
// every chunk read is aligned by construction rather than by remembering to.
// Group data lives in its own sections for the same reason: adding it to the
// record would break that stride.
//
// See docs/file-formats.md for field encodings and validation rules.

#include <cstddef>
#include <cstdint>

namespace library {

inline constexpr char CLIX_MAGIC[4] = {'C', 'L', 'X', '1'};
// Bumping this is the whole migration: an index from an older version fails
// validation and is rebuilt. No previous development format is accepted.
inline constexpr uint8_t CLIX_FORMAT_VERSION = 3;

// Bump when the fold or a permutation's sort key changes.
// Forces fold and ranks to be rebuilt while firstSeen values are preserved, so
// arrival history survives.
inline constexpr uint8_t CLIX_FOLD_VERSION = 4;

inline constexpr uint32_t CLIX_ALIGN = 512;
inline constexpr size_t CLIX_FOLD_BYTES = 96;
inline constexpr size_t CLIX_AUTHOR_KEY_BYTES = 12;

// A 2000-book card already produces a 429 KiB index. This hard bound keeps every
// record count and permutation ordinal representable by uint16_t.
inline constexpr uint16_t CLIX_MAX_RECORDS = 4096;

// Complete-path fingerprint stored in front of every record's name blob.
// Shared by the builder's reconciliation and the browser's recent-book lookup,
// which must agree byte-for-byte on the hash of the same path.
inline uint64_t clixPathHash(const char* data, const size_t len) {
  uint64_t hash = 14695981039346656037ULL;  // FNV-1a 64
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Persisted group kind
// Please keep these values stable (append only)
enum class GroupKind : uint8_t {
  None = 0,
  Series = 1,
  Publisher = 2,
  Language = 3,
  Subject = 4,
};
inline constexpr uint8_t GROUP_KIND_COUNT = static_cast<uint8_t>(GroupKind::Subject) + 1;
inline constexpr bool isKnownGroupKind(const uint8_t kind) { return kind < GROUP_KIND_COUNT; }

// Grouped builds store all four fields in kind order for reuse after a kind change
inline constexpr uint8_t GROUP_FIELD_COUNT = GROUP_KIND_COUNT - 1;
// Field index in the name blob. Requires kind != None
inline constexpr uint8_t groupFieldIndex(const GroupKind kind) { return static_cast<uint8_t>(kind) - 1; }

// Sentinel for an ungrouped book; zero is a valid group ID
inline constexpr uint16_t CLIX_GROUP_NONE = 0xFFFF;

// Series positions use hundredths (250 means 2.5). Zero is valid
// Other group kinds and unnumbered series books use GROUP_POSITION_NONE
inline constexpr uint16_t GROUP_POSITION_NONE = 0xFFFF;
// Highest representable position = 655.34. Anything bigger is clamped
inline constexpr uint16_t GROUP_POSITION_MAX = 0xFFFE;
inline constexpr size_t CLIX_GROUP_ENTRY_BYTES = 64;
inline constexpr size_t CLIX_GROUP_NAME_BYTES = CLIX_GROUP_ENTRY_BYTES - 3;
// Maximum source field length accepted by blob readers; the length prefix is one byte
inline constexpr size_t CLIX_SOURCE_GROUP_BYTES = 255;

enum ClixFlags : uint8_t {
  CLIX_FLAG_RANKS_DEGRADED = 1 << 0,
  CLIX_FLAG_DEDUP_DEGRADED = 1 << 1,
  // group table allocation failed; counts are zero but kind and source fields are retained
  CLIX_FLAG_GROUPS_DEGRADED = 1 << 2,
};

enum ClixMetadataStatus : uint8_t {
  CLIX_METADATA_NOT_ATTEMPTED = 0,
  CLIX_METADATA_EXTRACTED = 1,
  CLIX_METADATA_FAILED = 2,
};

#pragma pack(push, 1)

struct ClixHeader {
  char magic[4];
  uint8_t formatVersion;
  uint8_t foldVersion;
  uint8_t flags;
  uint8_t metadataEnabled;
  uint16_t bookCount;
  uint16_t folderCount;
  uint16_t nextFirstSeen;
  uint16_t groupCount;
  uint32_t folderStart;
  uint32_t folderLen;
  uint32_t recordStart;
  uint32_t permStart;
  uint32_t groupStart;
  uint32_t groupRefStart;
  uint32_t nameStart;
  uint32_t nameLen;
  // Expected total file size. Comparing it with the real size is a free
  // truncation guard: a build interrupted by a power cut cannot pass.
  uint32_t selfSize;
  // Number of grouped books, also the start of ungrouped books in group order
  uint16_t groupedCount;
  // Requested GroupKind, retained when group allocation fails
  uint8_t groupKind;
  uint8_t reserved[9];
};
static_assert(sizeof(ClixHeader) == 64, "ClixHeader must be exactly 64 bytes");

struct ClixRecord {
  uint32_t nameOff;   // from nameStart, into the per-record name blob
  uint32_t fileSize;  // captured while the dirent was open; part of the identity
  uint16_t firstSeen;
  uint16_t folderId;
  uint8_t nameLen;
  uint8_t foldLen;
  uint8_t authorKeyLen;
  uint8_t metadataStatus;
  char fold[CLIX_FOLD_BYTES];
  char authorKey[CLIX_AUTHOR_KEY_BYTES];
  uint32_t modificationTime;
};
static_assert(sizeof(ClixRecord) == 128, "ClixRecord must be exactly 128 bytes");
static_assert(CLIX_ALIGN % sizeof(ClixRecord) == 0, "records must tile a 512-byte sector");

struct ClixFolderHeader {
  uint8_t pathLen;  // 1..255; the path bytes follow, with no trailing '/'
};
static_assert(sizeof(ClixFolderHeader) == 1, "ClixFolderHeader must be 1 byte");

// Group heading and stored book count at groupStart + 64 * id.
struct ClixGroupEntry {
  uint16_t bookCount;
  uint8_t nameLen;
  char name[CLIX_GROUP_NAME_BYTES];  // UTF-8 truncated at a codepoint boundary (no terminating NUL)
};
static_assert(sizeof(ClixGroupEntry) == CLIX_GROUP_ENTRY_BYTES, "ClixGroupEntry must be exactly 64 bytes");
static_assert(CLIX_ALIGN % sizeof(ClixGroupEntry) == 0, "group entries must tile a 512-byte sector");

// Per-book group reference at groupRefStart + 4 * ordinal
// (stored separately to keep ClixRecord at 128 bytes)
struct ClixGroupRef {
  uint16_t groupId;  // CLIX_GROUP_NONE for ungrouped books
  // Series position in hundredths; GROUP_POSITION_NONE for other kinds or missing positions
  uint16_t position;
};
static_assert(sizeof(ClixGroupRef) == 4, "ClixGroupRef must be exactly 4 bytes");

#pragma pack(pop)

inline uint32_t alignUp(const uint32_t value) { return (value + CLIX_ALIGN - 1) / CLIX_ALIGN * CLIX_ALIGN; }

// Fill in every offset and the expected file size from the counts alone, so the
// writer and the reader can never disagree about where a section starts.
//
// Note: needs both h.bookCount and h.groupCount to be set before calling
inline void layoutSections(ClixHeader& h, const uint32_t folderBytes, const uint32_t nameBytes) {
  const uint32_t books = h.bookCount;
  h.folderStart = CLIX_ALIGN;
  h.folderLen = folderBytes;
  h.recordStart = alignUp(h.folderStart + folderBytes);
  h.permStart = alignUp(h.recordStart + books * sizeof(ClixRecord));
  h.groupStart = alignUp(h.permStart + books * 3u * sizeof(uint16_t));
  h.groupRefStart = alignUp(h.groupStart + static_cast<uint32_t>(h.groupCount) * sizeof(ClixGroupEntry));
  h.nameStart = alignUp(h.groupRefStart + books * sizeof(ClixGroupRef));
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
inline uint32_t groupOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + (2u * static_cast<uint32_t>(h.bookCount) + k) * sizeof(uint16_t);
}
inline uint32_t groupEntryOffset(const ClixHeader& h, const uint16_t groupId) {
  return h.groupStart + static_cast<uint32_t>(groupId) * sizeof(ClixGroupEntry);
}
inline uint32_t groupRefOffset(const ClixHeader& h, const uint16_t ordinal) {
  return h.groupRefStart + static_cast<uint32_t>(ordinal) * sizeof(ClixGroupRef);
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
  if (h.metadataEnabled > 1) return ClixValidity::SectionsInconsistent;
  if (actualFileSize != h.selfSize) return ClixValidity::SizeMismatch;

  // Both lengths are attacker-controlled bytes. Capped against the real file
  // size they cannot wrap the 32-bit section sums below, so the layout
  // comparison stays sound instead of re-deriving the same wrapped values.
  if (h.folderLen > actualFileSize || h.nameLen > actualFileSize) return ClixValidity::SectionsInconsistent;

  if (!isKnownGroupKind(h.groupKind)) return ClixValidity::SectionsInconsistent;
  if (h.metadataEnabled == 0 && h.groupKind != static_cast<uint8_t>(GroupKind::None))
    return ClixValidity::SectionsInconsistent;
  if (h.groupKind == static_cast<uint8_t>(GroupKind::None) && (h.groupCount != 0 || h.groupedCount != 0))
    return ClixValidity::SectionsInconsistent;
  if ((h.flags & CLIX_FLAG_GROUPS_DEGRADED) != 0 &&
      (h.groupKind == static_cast<uint8_t>(GroupKind::None) || h.groupCount != 0 || h.groupedCount != 0)) {
    return ClixValidity::SectionsInconsistent;
  }
  if (h.groupCount > h.bookCount) return ClixValidity::CountOutOfRange;
  if (h.groupedCount > h.bookCount) return ClixValidity::CountOutOfRange;
  if (h.groupCount == 0 && h.groupedCount != 0) return ClixValidity::CountOutOfRange;
  // each group contains at least one book.
  if (h.groupCount > h.groupedCount) return ClixValidity::CountOutOfRange;

  ClixHeader expected = h;
  layoutSections(expected, h.folderLen, h.nameLen);
  if (expected.folderStart != h.folderStart || expected.recordStart != h.recordStart ||
      expected.permStart != h.permStart || expected.groupStart != h.groupStart ||
      expected.groupRefStart != h.groupRefStart || expected.nameStart != h.nameStart ||
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
