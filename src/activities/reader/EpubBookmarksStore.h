#pragma once

#include <Epub.h>

#include <cstdint>
#include <vector>

struct EpubBookmark {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
};

namespace EpubBookmarksStore {

static constexpr uint8_t BOOKMARKS_VERSION = 1;
static constexpr size_t MAX_BOOKMARKS = 64;

// Load bookmarks from <cache>/bookmarks.bin. Returns true if file is valid/readable.
// Missing file is treated as success with an empty list.
bool load(const Epub& epub, std::vector<EpubBookmark>& outBookmarks);

// Save bookmarks to <cache>/bookmarks.bin (truncates). Returns true on success.
bool save(const Epub& epub, const std::vector<EpubBookmark>& bookmarks);

// Add (spine,page) if missing, then persist. Returns true on success.
bool add(const Epub& epub, uint16_t spineIndex, uint16_t pageNumber);

// Remove (spine,page) if present, then persist. Returns true on success.
bool remove(const Epub& epub, uint16_t spineIndex, uint16_t pageNumber);

// Returns true if a matching bookmark exists (based on current file contents).
bool contains(const Epub& epub, uint16_t spineIndex, uint16_t pageNumber);

}  // namespace EpubBookmarksStore
