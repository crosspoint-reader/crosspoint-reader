#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

struct FileEntry {
  std::string name;      // display name (no leading path, no trailing '/' on dirs)
  std::string realPath;  // non-empty only for virtual entries (e.g. /Feed); otherwise basepath+name
  uint32_t modTime;      // FAT packed (date<<16|time); higher = newer; 0 if unavailable
  bool isDirectory;
};

class MyLibraryActivity final : public Activity {
 private:
  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Files state
  std::string basepath = "/";
  std::vector<FileEntry> files;
  bool sortByDate = true;  // true = newest first; false = alphabetical

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("MyLibrary", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
