#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 private:
  struct FileEntry {
    std::string name;   // filename; trailing '/' = directory
    uint32_t size;      // file size in bytes; 0 for directories
    uint32_t dateTime;  // FAT modify timestamp packed as (date << 16) | time; 0 = unknown
  };

  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool lockLongPressBack = false;

  // Files state
  std::string basepath = "/";
  std::vector<FileEntry> files;

  // Data loading
  void loadFiles();
  static void sortFileList(std::vector<FileEntry>& entries);
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
