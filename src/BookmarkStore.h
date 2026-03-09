#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Stores starred/bookmarked pages for a single book.
// Persisted as a binary file on SD card within the book's cache directory.
class BookmarkStore {
 public:
  struct Bookmark {
    uint16_t spineIndex;
    uint16_t pageNumber;
  };

  // Load bookmarks from the cache directory (e.g. .crosspoint/epub_<hash>/).
  void load(const std::string& cachePath) {
    basePath = cachePath;
    bookmarks.clear();
    dirty = false;

    FsFile f;
    if (!Storage.openFileForRead("BKM", getFilePath(), f)) {
      return;
    }

    uint8_t version;
    serialization::readPod(f, version);
    if (version != FILE_VERSION) {
      LOG_DBG("BKM", "Bookmark version mismatch (%d != %d), ignoring", version, FILE_VERSION);
      f.close();
      return;
    }

    uint16_t count;
    serialization::readPod(f, count);
    bookmarks.reserve(count);

    for (uint16_t i = 0; i < count; i++) {
      Bookmark bm;
      serialization::readPod(f, bm.spineIndex);
      serialization::readPod(f, bm.pageNumber);
      bookmarks.push_back(bm);
    }

    f.close();
    LOG_DBG("BKM", "Loaded %d bookmarks", count);
  }

  // Save bookmarks to SD card (only if changed).
  void save() {
    if (!dirty || basePath.empty()) {
      return;
    }

    FsFile f;
    if (!Storage.openFileForWrite("BKM", getFilePath(), f)) {
      LOG_ERR("BKM", "Failed to save bookmarks");
      return;
    }

    serialization::writePod(f, FILE_VERSION);
    const uint16_t count = static_cast<uint16_t>(bookmarks.size());
    serialization::writePod(f, count);

    for (const auto& bm : bookmarks) {
      serialization::writePod(f, bm.spineIndex);
      serialization::writePod(f, bm.pageNumber);
    }

    f.close();
    dirty = false;
    LOG_DBG("BKM", "Saved %d bookmarks", count);
  }

  // Toggle bookmark for the given page. Returns true if now starred, false if removed.
  bool toggle(uint16_t spineIndex, uint16_t pageNumber) {
    auto it = find(spineIndex, pageNumber);
    if (it != bookmarks.end()) {
      bookmarks.erase(it);
      dirty = true;
      return false;
    }
    bookmarks.push_back({spineIndex, pageNumber});
    dirty = true;
    return true;
  }

  // Check if a page is starred.
  [[nodiscard]] bool has(uint16_t spineIndex, uint16_t pageNumber) const {
    return std::any_of(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const Bookmark& bm) {
      return bm.spineIndex == spineIndex && bm.pageNumber == pageNumber;
    });
  }

  [[nodiscard]] const std::vector<Bookmark>& getAll() const { return bookmarks; }
  [[nodiscard]] bool isEmpty() const { return bookmarks.empty(); }

 private:
  static constexpr uint8_t FILE_VERSION = 1;

  std::vector<Bookmark> bookmarks;
  std::string basePath;
  bool dirty = false;

  [[nodiscard]] std::string getFilePath() const { return basePath + "/bookmarks.bin"; }

  std::vector<Bookmark>::iterator find(uint16_t spineIndex, uint16_t pageNumber) {
    return std::find_if(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const Bookmark& bm) {
      return bm.spineIndex == spineIndex && bm.pageNumber == pageNumber;
    });
  }
};
