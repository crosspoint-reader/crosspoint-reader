#pragma once

#include <HalStorage.h>
#include <Logging.h>

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
    if (f.read(reinterpret_cast<uint8_t*>(&version), sizeof(version)) != sizeof(version) || version != FILE_VERSION) {
      f.close();
      return;
    }

    uint16_t count;
    if (f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count) || count > MAX_BOOKMARKS) {
      LOG_ERR("BKM", "Invalid bookmark count: %u", static_cast<unsigned>(count));
      f.close();
      return;
    }

    bookmarks.reserve(count);
    for (uint16_t i = 0; i < count; i++) {
      Bookmark bm;
      if (f.read(reinterpret_cast<uint8_t*>(&bm.spineIndex), sizeof(bm.spineIndex)) != sizeof(bm.spineIndex) ||
          f.read(reinterpret_cast<uint8_t*>(&bm.pageNumber), sizeof(bm.pageNumber)) != sizeof(bm.pageNumber)) {
        LOG_ERR("BKM", "Truncated bookmarks file at entry %d", i);
        bookmarks.clear();
        f.close();
        return;
      }
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

    if (bookmarks.size() > UINT16_MAX) {
      LOG_ERR("BKM", "Too many bookmarks to save: %u", static_cast<unsigned>(bookmarks.size()));
      return;
    }

    FsFile f;
    if (!Storage.openFileForWrite("BKM", getFilePath(), f)) {
      LOG_ERR("BKM", "Failed to save bookmarks");
      return;
    }

    auto writePodChecked = [&f](const auto& value) {
      return f.write(reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
    };

    const uint16_t count = static_cast<uint16_t>(bookmarks.size());
    bool ok = writePodChecked(FILE_VERSION) && writePodChecked(count);

    for (const auto& bm : bookmarks) {
      ok = ok && writePodChecked(bm.spineIndex) && writePodChecked(bm.pageNumber);
    }

    ok = ok && f.close();
    if (!ok) {
      LOG_ERR("BKM", "Failed while writing bookmarks");
      return;
    }
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
  void markDirty() { dirty = true; }

 private:
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr uint16_t MAX_BOOKMARKS = 1000;

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
