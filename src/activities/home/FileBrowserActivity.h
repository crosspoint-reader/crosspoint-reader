#pragma once

#include <FileIndex.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Names live back-to-back in nameArena (NUL-terminated, trailing '/' marks a
  // directory); entries hold offsets. One shared block instead of a heap
  // string per entry keeps directory loads to two allocations total.
  struct FileEntry {
    uint32_t nameOffset;  // offset into nameArena
    uint32_t size;        // file size in bytes; 0 for directories
    uint32_t dateTime;    // FAT timestamp packed as (date << 16) | time; 0 = unknown
  };

  // Deletion
  bool removeDirFile(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Books;

  // Files state. Folders up to the index threshold are listed and sorted in
  // RAM (`files`); larger folders switch to the on-SD FileIndex backend so RAM
  // use stays bounded. The entryCount/entryNameAt accessors hide which backend
  // is active.
  std::string basepath = "/";
  std::vector<FileEntry> files;
  std::vector<char> nameArena;
  std::unique_ptr<char[]> fileNameBuffer;
  std::unique_ptr<FileIndex> fileIndex;
  std::unique_ptr<FileIndex::Entry> indexEntry;  // scratch record for index reads
  static constexpr size_t INDEX_PAGE_MAX_ENTRIES = FileIndex::MAX_PAGE_ENTRIES;
  // Fast path for 255-byte card names, including a directory marker and NUL.
  static constexpr size_t INDEX_PAGE_NAME_STRIDE = 257;
  static constexpr size_t INDEX_PAGE_NAME_BYTES = INDEX_PAGE_MAX_ENTRIES * INDEX_PAGE_NAME_STRIDE;
  struct IndexPageEntry {
    uint16_t nameOffset;
  };
  std::unique_ptr<char[]> indexPageNames;
  IndexPageEntry indexPageEntries[INDEX_PAGE_MAX_ENTRIES] = {};
  size_t indexPageFirst = SIZE_MAX;
  size_t indexPageCount = 0;
  size_t indexPageSpan = 0;
  bool indexPageCacheUnavailable = false;
  std::string indexCachedName;
  size_t indexCachedRow = SIZE_MAX;
  bool usingIndex = false;
  bool sortDescending = false;

  // Data loading
  void loadFiles();
  bool loadFilesIntoVector(size_t cap, bool& overflow);
  void sortFileList();
  size_t entryCount() const;
  bool prepareIndexPage(size_t first, size_t count);
  void clearIndexPageCache();
  // Name of the entry at `row` in display order; directories carry a trailing
  // '/' in both backends. Indexed pointers are consumed synchronously before
  // the visible-page cache is repopulated.
  const char* entryNameAt(size_t row);
  size_t findEntry(const std::string& name);

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
