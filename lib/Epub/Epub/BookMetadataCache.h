#pragma once

#include <BufferedFileIO.h>
#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "HashUtils.h"

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
    std::string series;
    std::string seriesIndex;
    std::string description;
    // Not stored in book.bin: its version would have to go up and every book be cached again. Set by
    // a fresh OPF parse and by the metadata sidecar; BookDetailsLookup::parse() loads past book.bin
    // to get them.
    std::string primaryAuthor;
    std::string authorSort;
    std::string isbn;
    std::string asin;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool tocReliable;
  bool loaded;
  bool buildMode;

  FsFile bookFile;
  // Temp file handles during build
  FsFile spineFile;
  FsFile tocFile;
  // Buffered views over the temp files during their write phases (createSpineEntry /
  // createTocEntry stream one small record per manifest itemref / TOC entry — unbuffered,
  // each field is a separate ~1.5 ms FsFile call). Engaged by the begin*Pass methods,
  // flushed and dropped by the matching end*Pass. Degrade internally to pass-through on OOM.
  std::optional<serialization::BufferedFileWriter> spineWriter_;
  std::optional<serialization::BufferedFileWriter> tocWriter_;

  // Index for fast href→spineIndex lookup (used only for large EPUBs): 16 B/spine, sorted for
  // binary search, allocated NOTHROW in one block. First-open indexing runs with the secondary
  // framebuffer lent rather than freed, on ~50 KB of fragmented heap, and a ~3000-spine book's
  // index does not fit there: the deque that used to hold it allocated through the throwing
  // operator new and aborted the device in beginTocPass(). When the block is refused, lookups
  // take the resumed scan below, which needs no memory.
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  // spineIndex is int16_t (-1 means "no match"), so spine indices must fit in a positive int16_t.
  // spineCount is uint16_t and would silently wrap past this; no real EPUB approaches it, but the
  // limit should be stated rather than latent.
  static constexpr int MAX_ADDRESSABLE_SPINES = 32767;
  std::unique_ptr<SpineHrefIndexEntry[]> spineHrefIndex;
  bool useSpineHrefIndex = false;
  // Without the index, a lookup scans the spine file starting after the previous match and wraps
  // once, instead of starting at the first spine. A TOC lists its chapters in reading order, so
  // the entry it wants is usually the next record: a 2956-entry TOC scanned from the start read
  // ~4.4 million spine records. The scan reads through a buffered view (an unbuffered field read
  // is a full FsFile call), and the resume point is the index and byte offset of the next record.
  std::optional<serialization::BufferedFileReader> spineScanReader_;
  int tocScanNextIndex_ = 0;
  uint32_t tocScanNextOffset_ = 0;
  // The index or the scan reader, set up by the first createTocEntry() of a TOC pass.
  bool tocLookupPrepared_ = false;
  // TOC entries of this pass whose href matched a spine item (spineIndex >= 0). Build-pass only,
  // never written to book.bin: Epub::load compares it between the nav and the NCX.
  uint16_t tocMappedCount_ = 0;
  void prepareTocLookup();
  int16_t findSpineByScan(const std::string& href);

  // Batch ZIP size lookup and fast spine-href index are always better when N is
  // larger than a handful — lower threshold so even moderate books (e.g. 105
  // spine items) take the O(n·log m) batch path instead of O(n·m) per-item scans.
  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 16;

  uint32_t writeSpineEntry(FsFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(FsFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(FsFile& file) const;
  TocEntry readTocEntry(FsFile& file) const;
  // Out-parameter overloads reuse the caller's string capacity inside hot
  // build loops, eliminating per-iteration std::string allocation churn that
  // would otherwise fragment the heap during book.bin construction.
  void readSpineEntry(FsFile& file, SpineEntry& out) const;
  void readTocEntry(FsFile& file, TocEntry& out) const;
  // Buffered counterparts for the build loops (identical wire format).
  uint32_t writeSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const;
  uint32_t writeTocEntry(serialization::BufferedFileWriter& out, const TocEntry& entry) const;
  void readSpineEntry(serialization::BufferedFileReader& in, SpineEntry& out) const;
  void readTocEntry(serialization::BufferedFileReader& in, TocEntry& out) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)),
        lutOffset(0),
        spineCount(0),
        tocCount(0),
        tocReliable(false),
        loaded(false),
        buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  bool resetTocPassOutput();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // True when the spine/TOC cache file (book.bin) already exists for this cachePath,
  // i.e. load() can read it instead of rebuilding. Cheap (a single Storage.exists).
  static bool cacheExists(const std::string& cachePath);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  // Build-pass only (see tocMappedCount_); 0 in read mode.
  int getTocMappedCount() const { return tocMappedCount_; }
  bool isTocReliable() const { return tocReliable; }
  bool isLoaded() const { return loaded; }
  // Mark the cache "loaded" after a METADATA-ONLY OPF parse populated coreMetadata (title, author,
  // series, coverItemHref, ...) without building the spine/TOC book.bin. Lets the isLoaded() gates
  // pass while spineCount/tocCount stay 0 — the caller must NOT use the spine/TOC accessors in this
  // state. Used by Epub::loadForCover() (avoids a full 1732-spine book.bin build just to show a
  // thumbnail) and Epub::loadForMetadata() (same, for a series-sequel folder scan).
  void markCoverMetadataLoaded() { loaded = true; }
};
