#pragma once

// Read side of the CLX1 index.
//
// Holds one open file handle and a few hundred bytes; nothing that scales with
// the library stays resident. A page of rows is a handful of seeks, which is
// what the fixed 128-byte record stride buys — record k is always at
// recordStart + 128k, so no offset table has to be loaded to find it.

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

enum class SortOrder : uint8_t {
  // "Recent" orders by file modification time (arrival on the card), oldest
  // first in Asc; firstSeen breaks ties for filesystems without timestamps.
  RecentAsc,
  RecentDesc,
  TitleAsc,
  TitleDesc,
  AuthorAsc,
  AuthorDesc,
  GroupAsc,
  GroupDesc,
};

// One book to locate in the index: the complete-path hash (clixPathHash) is
// the identity; fileSize, when nonzero, is a cheap in-record prefilter that
// avoids reading the hash blob for most records. Zero means size unknown and
// every record's hash is checked.
struct BookIdentity {
  uint64_t pathHash;
  uint32_t fileSize;
};

// Stored group fields in groupFieldIndex() order: series, publisher, language tag, subject.
// seriesPosition belongs to the series, regardless of the configured kind.
struct SourceFields {
  uint16_t seriesPosition = GROUP_POSITION_NONE;
  std::string field[GROUP_FIELD_COUNT];
};

class LibraryIndexFile {
 public:
  LibraryIndexFile() = default;
  ~LibraryIndexFile();
  LibraryIndexFile(const LibraryIndexFile&) = delete;
  LibraryIndexFile& operator=(const LibraryIndexFile&) = delete;

  // Open and validate. On failure `validity()` says why, so a rebuild loop
  // caused by a format bug is visible in the log rather than looking like a slow
  // first boot.
  bool open(const char* path);
  // Accept an otherwise valid stale fold so a rebuild can preserve arrival
  // history without exposing stale sort/search keys to the browser.
  bool openForReconciliation(const char* path);
  void close();
  bool isOpen() const { return opened; }
  bool ioFailed() const { return readFailed; }

  ClixValidity validity() const { return lastValidity; }
  const ClixHeader& header() const { return head; }
  uint16_t bookCount() const { return opened ? head.bookCount : 0; }
  bool ranksDegraded() const { return opened && (head.flags & CLIX_FLAG_RANKS_DEGRADED) != 0; }
  bool dedupDegraded() const { return opened && (head.flags & CLIX_FLAG_DEDUP_DEGRADED) != 0; }
  bool groupsDegraded() const { return opened && (head.flags & CLIX_FLAG_GROUPS_DEGRADED) != 0; }
  uint16_t groupCount() const { return opened ? head.groupCount : 0; }
  // Number of grouped books and start of the ungrouped block in group order.
  uint16_t groupedCount() const { return opened ? head.groupedCount : 0; }
  // Kind stored in the index, including when group allocation failed.
  GroupKind groupKind() const { return opened ? static_cast<GroupKind>(head.groupKind) : GroupKind::None; }
  bool hasGroups() const { return opened && head.groupCount > 0 && head.groupedCount > 0; }

  // Record ordinal of the row at display position `row` in `order`. Returns
  // 0xFFFF when out of range, which callers treat as "no such row" rather than
  // indexing anyway.
  uint16_t ordinalForRow(SortOrder order, uint16_t row);

  // Display rows (RecentAsc space) of up to MAX_IDENTITY_LOOKUPS books, 0xFFFF
  // for books not in the index. One chunked pass over the record section plus
  // one over the arrival permutation, so cost is bounded by the library, not by
  // `count` — callers batch their lookups instead of calling per book.
  static constexpr size_t MAX_IDENTITY_LOOKUPS = 16;
  bool recentRowsFor(const BookIdentity* books, size_t count, uint16_t* outRows);

  bool readRecord(uint16_t ordinal, ClixRecord& out);
  // Persisted complete-path fingerprint used by rebuild reconciliation.
  bool readPathHash(const ClixRecord& record, uint64_t& out);

  // Display basename, exactly as it sits on the card. This is the only string
  // the UI draws, and it is never shortened on disk.
  bool readName(const ClixRecord& record, std::string& out);
  // The author the build settled on, stored right after the name. Reading it
  // rather than re-deriving it from the name is what makes the metadata pass and
  // the spelling harmonisation visible: neither survives a filename that no
  // longer carries "Title - Author".
  bool readAuthor(const ClixRecord& record, std::string& out);
  bool readTitle(const ClixRecord& record, std::string& out);
  // Cleaned author spelling before the library-wide spelling vote. Empty is a
  // valid value, so success is independent of `out.empty()`.
  bool readSourceAuthor(const ClixRecord& record, std::string& out);
  // Read all four fields for reuse when the grouping setting changes.
  bool readSourceFields(const ClixRecord& record, SourceFields& out);
  // Read the stored source value of the configured field, before heading truncation.
  // Returns true with an empty output for None or a missing value.
  bool readSourceGroup(const ClixRecord& record, std::string& out);
  // Read search fields in one forward pass. title may be null to skip it.
  // Missing fields yield empty strings. Returns false on a malformed blob or I/O
  // failure; fields read before the failure remain available.
  bool readSearchFields(const ClixRecord& record, std::string* title, std::string& author, std::string& sourceGroup);

  // Absolute path of the book, rebuilt from its folder record.
  bool readPath(const ClixRecord& record, std::string& out);

  // Read the group reference; ungrouped books return CLIX_GROUP_NONE.
  bool readGroupRef(uint16_t ordinal, ClixGroupRef& out);
  // Read a group heading and book count. Returns false for an out-of-range ID.
  bool readGroup(uint16_t groupId, std::string& name, uint16_t& bookCount);

 private:
  class BlobWindow;

  bool openImpl(const char* path, bool acceptStaleFold);
  bool readAt(uint32_t offset, void* dst, size_t len);
  // Offset of the first length-prefixed field, after the path hash and filename.
  bool blobTailStart(const ClixRecord& record, uint32_t& at) const;
  // Read one u8-prefixed field, or skip it when out is null.
  bool takeShortField(BlobWindow& blob, std::string* out);
  bool takeSourceGroup(BlobWindow& blob, std::string& out);
  bool readBlobField(const ClixRecord& record, uint8_t field, std::string& out);

  HalFile file;
  ClixHeader head{};
  bool opened = false;
  bool readFailed = false;
  ClixValidity lastValidity = ClixValidity::BadMagic;
};

}  // namespace library
