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
// Bumping this is the whole migration: an index from an older version fails
// validation and is rebuilt. This format first lands from this branch as v1.
inline constexpr uint8_t CLIX_FORMAT_VERSION = 1;

// Bump when the fold or the article table changes. Forces fold and ranks to be
// rebuilt while firstSeen values are preserved, so "recently added" survives.
inline constexpr uint8_t CLIX_FOLD_VERSION = 1;

inline constexpr uint32_t CLIX_ALIGN = 512;
inline constexpr size_t CLIX_FOLD_BYTES = 96;
inline constexpr size_t CLIX_AUTHOR_KEY_BYTES = 12;

// A 2000-book card already produces a 429 KiB index. This hard bound keeps every
// record count and permutation ordinal representable by uint16_t.
inline constexpr uint16_t CLIX_MAX_RECORDS = 4096;

enum ClixFlags : uint8_t {
  CLIX_FLAG_RANKS_DEGRADED = 1 << 0,
  CLIX_FLAG_DEDUP_DEGRADED = 1 << 1,
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
  uint16_t padding1;
  uint32_t folderStart;
  uint32_t folderLen;
  uint32_t recordStart;
  uint32_t permStart;
  uint32_t nameStart;
  uint32_t nameLen;
  // Expected total file size. Comparing it with the real size is a free
  // truncation guard: a build interrupted by a power cut cannot pass.
  uint32_t selfSize;
  uint8_t reserved[20];
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

#pragma pack(pop)

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
  return ClixValidity::Ok;
}

const char* clixValidityName(ClixValidity v);

}  // namespace library
