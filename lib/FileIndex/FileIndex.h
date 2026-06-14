#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

/**
 * On-SD directory index for browsing folders with bounded RAM.
 *
 * One index file per directory (under /.crosspoint/fileindex/), holding every
 * accepted entry's metadata plus a sort-order table built with an external
 * (bounded-RAM) merge sort. Browsing reads only the visible rows back from SD,
 * so RAM use is independent of how many files the folder holds.
 *
 * Staleness is detected by re-enumerating the directory on open() and hashing
 * (name, size, timestamp, isDir) of every accepted entry into a signature; a
 * mismatch with the stored header (or a different sort mode / filter result)
 * triggers a rebuild. Rebuilds write to a temp file and atomically rename, so
 * a power loss never leaves a corrupt index in place.
 *
 * Layout: [IndexHeader][dir path][records in enumeration order][sorted offsets]
 * The offsets table stores directories first, then files, each section sorted
 * ascending; descending views just read the sections backwards.
 */
class FileIndex {
 public:
  // Mirrors CrossPointSettings::FILE_SORT_MODE without depending on app headers.
  enum class SortMode : uint8_t { Name = 0, Date = 1, Size = 2, Type = 3 };

  // Longest name we reproduce from the index (FAT LFN is 255 UTF-16 units,
  // worst-case ~765 UTF-8 bytes; the browser's own name buffer is 500).
  static constexpr size_t MAX_NAME = 511;

  struct Entry {
    uint32_t size;
    uint32_t dateTime;  // FAT (date << 16) | time, max(mtime, ctime)
    bool isDir;
    char name[MAX_NAME + 1];
  };

  // Filter applied identically to the staleness scan and the build, so filter
  // changes (e.g. show-hidden toggled) naturally change the signature and
  // trigger a rebuild. Must be a stateless function.
  using AcceptFn = bool (*)(const char* name, bool isDir);

  FileIndex() = default;
  ~FileIndex() { close(); }
  FileIndex(const FileIndex&) = delete;
  FileIndex& operator=(const FileIndex&) = delete;

  // Validate-or-rebuild the index for dirPath. Returns false on IO failure
  // (caller should fall back to a capped in-RAM listing).
  bool open(const char* dirPath, SortMode sortMode, AcceptFn accept);
  void close();
  bool isOpen() const { return opened; }

  size_t dirCount() const { return hdr.dirCount; }
  size_t fileCount() const { return hdr.fileCount; }
  size_t totalCount() const { return hdr.dirCount + hdr.fileCount; }

  // Fetch the entry shown at `row` (0-based). Directories always occupy the
  // first dirCount() rows; `descending` reverses order within each section.
  bool entryAt(size_t row, bool descending, Entry& out);

  // Read one visible browser page. Names are written into fixed-size slots in
  // display order, but records are fetched in blob-offset order so pages whose
  // files were created together need only sequential SD access.
  static constexpr size_t MAX_PAGE_ENTRIES = 22;
  bool pageNamesAt(size_t firstRow, size_t count, bool descending, char* names, size_t nameStride);

  // Row of the entry with this exact name (as shown with `descending`), or
  // SIZE_MAX if not present. Streams the index; used for pre-selection.
  size_t findRowByName(const char* name, bool descending);

 private:
#pragma pack(push, 1)
  struct IndexHeader {
    char magic[4];
    uint8_t version;
    uint8_t sortMode;
    uint16_t pathLen;
    uint32_t dirSignature;
    uint32_t dirCount;
    uint32_t fileCount;
    uint32_t blobStart;
    uint32_t blobLen;
    uint32_t offsetsStart;
  };
  struct RecordHeader {
    uint32_t size;
    uint32_t dateTime;
    uint8_t flags;  // bit0 = directory
    uint8_t reserved;
    uint16_t nameLen;
  };
  // External-sort element: fixed-size key prefix + record offset. key[0] is the
  // section byte (1 = dir, 2 = file) so one sort pass yields dirs-then-files;
  // the payload starts with the numeric field (date/size modes, big-endian so
  // memcmp orders numerically) followed by a truncated FsHelpers::naturalSortKey
  // of the name. Primary runs use blobOffset for deterministic fixed-key ties;
  // writeOffsets resolves them against the full names.
  struct RunRecord {
    uint8_t key[28];
    uint32_t blobOffset;
  };
#pragma pack(pop)
  static_assert(sizeof(RunRecord) == 32, "run record packing");

  struct BuildState;  // scratch handles + chunk buffer, heap-allocated per build

  bool scanDirectory(const char* dirPath, AcceptFn accept, uint32_t& signature, uint32_t& dirs, uint32_t& files);
  bool loadExisting(const char* dirPath, SortMode sortMode, uint32_t signature, uint32_t dirs, uint32_t files);
  bool build(const char* dirPath, SortMode sortMode, AcceptFn accept, uint32_t signature, uint32_t dirs,
             uint32_t files);
  bool flushChunk(BuildState& bs);
  bool mergeRuns(BuildState& bs, uint32_t recordCount);
  bool writeOffsets(BuildState& bs, uint32_t recordCount);
  bool sortSmallTieGroup(BuildState& bs, size_t count, uint32_t& appendPos);
  bool writeInitialTieRun(BuildState& bs, HalFile& out, uint32_t offsetA, bool hasB, uint32_t offsetB, bool useChunk);
  bool mergeTieRuns(BuildState& bs, uint32_t runCount, const char*& finalPath);
  bool appendTieOffsets(BuildState& bs, const char* path, uint32_t recordCount, uint32_t& appendPos);
  bool finishTieGroup(BuildState& bs, HalFile& tieOut, bool usesScratch, uint32_t groupCount, uint32_t initialRunCount,
                      bool hasPending, uint32_t pendingOffset, uint32_t& appendPos);
  void makeKey(RunRecord& rec, SortMode sortMode, bool isDir, uint32_t size, uint32_t dateTime, const char* name) const;
  bool readNameAt(HalFile& file, uint32_t recordOffset, char* out, size_t cap, uint16_t* nameLen = nullptr);

  bool readOffsetForPhysIndex(size_t physIndex, uint32_t& recordOffset);

  HalFile idxFile;  // kept open read-only while browsing
  IndexHeader hdr{};
  bool opened = false;
  char idxPath[64] = {0};
  std::unique_ptr<char[]> nameBuf;  // shared 512B name buffer for scans/lookups

  // Small cache of one offsets-table page to halve seeks during rendering
  static constexpr size_t OFFSETS_CACHE_ENTRIES = 64;
  uint32_t offsetsCache[OFFSETS_CACHE_ENTRIES] = {0};
  size_t offsetsCacheFirst = SIZE_MAX;

  // Parallel arrays avoid padding while keeping page-read scratch off the
  // small task stack. The FileIndex object itself is already heap allocated.
  uint32_t pageRecordOffsets[MAX_PAGE_ENTRIES] = {0};
  uint8_t pageOutputSlots[MAX_PAGE_ENTRIES] = {0};
};
