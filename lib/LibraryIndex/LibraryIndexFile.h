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
  AddedAsc,
  AddedDesc,
  TitleAsc,
  TitleDesc,
  AuthorAsc,
  AuthorDesc,
  SeriesAsc,
  SeriesDesc,
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

  ClixValidity validity() const { return lastValidity; }
  const ClixHeader& header() const { return head; }
  uint16_t bookCount() const { return opened ? head.bookCount : 0; }
  bool ranksDegraded() const { return opened && (head.flags & CLIX_FLAG_RANKS_DEGRADED) != 0; }
  bool dedupDegraded() const { return opened && (head.flags & CLIX_FLAG_DEDUP_DEGRADED) != 0; }
  // Whether the walk that wrote this index read book metadata. What the index
  // can hold depends on it — series come from nowhere else — so a reader whose
  // setting now disagrees knows the shelf is out of date rather than the card
  // untagged.
  bool usedMetadata() const { return opened && (head.flags & CLIX_FLAG_USED_METADATA) != 0; }
  uint16_t seriesCount() const { return opened ? head.seriesCount : 0; }
  // Books belonging to a series, which is also where the standalone block starts
  // in series order.
  uint16_t knownSeriesCount() const { return opened ? head.knownSeriesCount : 0; }
  // Whether a series order is worth offering at all. An index built before the
  // reader turned book metadata on carries no series, and a tab that leads to
  // nothing but ungrouped books is worse than no tab.
  bool hasSeries() const { return opened && head.seriesCount > 0 && head.knownSeriesCount > 0; }

  // Record ordinal of the row at display position `row` in `order`. Returns
  // 0xFFFF when out of range, which callers treat as "no such row" rather than
  // indexing anyway.
  uint16_t ordinalForRow(SortOrder order, uint16_t row);

  bool readRecord(uint16_t ordinal, ClixRecord& out);

  // Display basename, exactly as it sits on the card. This is the only string
  // the UI draws, and it is never shortened on disk.
  bool readName(const ClixRecord& record, std::string& out);
  // The author the build settled on, stored right after the name. Reading it
  // rather than re-deriving it from the name is what makes the metadata pass and
  // the spelling harmonisation visible: neither survives a filename that no
  // longer carries "Title - Author".
  bool readAuthor(const ClixRecord& record, std::string& out);
  bool readTitle(const ClixRecord& record, std::string& out);

  // Absolute path of the book, rebuilt from its folder record.
  bool readPath(const ClixRecord& record, std::string& out);

  // The book's series and position. Fills `out` with CLIX_SERIES_NONE when the
  // book belongs to none, so callers can read it unconditionally.
  bool readSeriesRef(uint16_t ordinal, ClixSeriesRef& out);
  // Name and on-card book count of one series. `seriesId` comes from a
  // ClixSeriesRef; anything out of range fails rather than reading a neighbour.
  bool readSeries(uint16_t seriesId, std::string& name, uint16_t& bookCount);

 private:
  bool openImpl(const char* path, bool acceptStaleFold);
  bool readAt(uint32_t offset, void* dst, size_t len);

  HalFile file;
  ClixHeader head{};
  bool opened = false;
  ClixValidity lastValidity = ClixValidity::BadMagic;
};

}  // namespace library
