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
  struct FileEntry {
    std::string name;   // filename; trailing '/' = directory
    uint32_t size;      // file size in bytes; 0 for directories
    uint32_t dateTime;  // FAT timestamp packed as (date << 16) | time; 0 = unknown
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
  std::unique_ptr<char[]> fileNameBuffer;
  std::unique_ptr<FileIndex> fileIndex;
  std::unique_ptr<FileIndex::Entry> indexEntry;  // scratch record for index reads
  std::string indexCachedName;
  size_t indexCachedRow = SIZE_MAX;
  bool usingIndex = false;
  bool sortDescending = false;

  // Data loading
  void loadFiles();
  bool loadFilesIntoVector(size_t cap, bool& overflow);
  static void sortFileList(std::vector<FileEntry>& entries);
  size_t entryCount() const;
  // Name of the entry at `row` in display order; directories carry a trailing
  // '/' in both backends. Returns a reference valid until the next call.
  const std::string& entryNameAt(size_t row);
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
