#pragma once

#include <BufferedFile.h>
#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
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
  bool loaded;
  bool buildMode;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;
  // Buffers the per-entry tmp-file writes during the OPF/TOC passes: those
  // writes interleave with zip-inflate SD reads, and unbuffered they thrash
  // SdFat's shared sector cache (one 512B transaction per 4-byte pod). One
  // wrapper serves whichever pass is active (spine, then toc).
  std::unique_ptr<serialization::BufferedFileWriter> passOut;

  // Cumulative spine sizes, cached in RAM at load() so progress/percent lookups are
  // O(1) instead of 2 seeks + a heap-allocating SpineEntry read per access (4 bytes
  // per spine item; <1KB for typical books).
  std::vector<uint32_t> cumulativeSizes;

  // Index for fast href→spineIndex lookup (used only for large EPUBs)
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(HalFile& file) const;
  TocEntry readTocEntry(HalFile& file) const;

#ifdef CROSSPOINT_BOOKBIN_PSRAM
  // Whole-file PSRAM mirror of the finalized book.bin. nullptr = not mirrored,
  // which is byte-for-byte the historical file path. See loadMirror().
  uint8_t* mirror = nullptr;
  uint32_t mirrorSize = 0;
  // Set by the first ensureMirror(), whether or not it produced a mirror, so a
  // book.bin that is too big (or a PSRAM refusal) is not re-probed on every
  // lookup. Cleared only by load(), which is the one path that can make a
  // previous verdict stale.
  bool mirrorLoadAttempted = false;
  // Sanity cap on what gets mirrored. Real book.bin files run from a few KB to
  // ~200KB (measured on a 1,732-spine omnibus); anything past this is absurd or
  // corrupt and stays on SD. Sized just past the worst case observed rather than
  // "surely enough": the cap's job is to refuse a corrupt length before it turns
  // into a multi-MB PSRAM allocation.
  static constexpr uint32_t MAX_MIRROR_BYTES = 512u * 1024;
  // Fill/free the mirror. Both are no-ops that leave bookFile untouched when the
  // allocation is refused, so every caller keeps working off the file handle.
  // ensureMirror() is the lazy one-shot entry point the getters call.
  void ensureMirror();
  void loadMirror();
  void releaseMirror();
#endif

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
#ifdef CROSSPOINT_BOOKBIN_PSRAM
  ~BookMetadataCache() { releaseMirror(); }
  // The mirror is a raw owning pointer; the class was never copied (HalFile
  // members are already move-only), this makes that a compile error rather than
  // a double free.
  BookMetadataCache(const BookMetadataCache&) = delete;
  BookMetadataCache& operator=(const BookMetadataCache&) = delete;
#else
  ~BookMetadataCache() = default;
#endif

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  // Cumulative byte size up to and including the given spine item (0 if out of range
  // or not loaded). Backed by the in-RAM cumulativeSizes cache populated in load().
  uint32_t getCumulativeSize(int index) const;
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};
