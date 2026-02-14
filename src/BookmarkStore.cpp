#include "BookmarkStore.h"

#include <SDCardManager.h>

#include <algorithm>

std::string BookmarkStore::getBookmarkPath(const std::string& bookPath) {
  // FNV-1a hash of full book path to avoid filename collisions
  uint32_t hash = 2166136261u;
  for (const char c : bookPath) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  char hexHash[9];
  snprintf(hexHash, sizeof(hexHash), "%08x", hash);
  return std::string(BOOKMARKS_DIR) + "/" + hexHash + ".bookmarks";
}

std::vector<BookmarkEntry> BookmarkStore::loadBookmarks(const std::string& bookPath) {
  std::vector<BookmarkEntry> entries;
  const std::string path = getBookmarkPath(bookPath);

  FsFile file;
  if (!SdMan.openFileForRead(TAG, path, file)) {
    return entries;
  }

  uint8_t header[2];
  const int bytesRead = file.read(header, 2);
  if (bytesRead != 2) {
    file.close();
    return entries;
  }
  if (header[0] != FORMAT_VERSION) {
    Serial.printf("[%lu] [%s] Skipping bookmark file with version %d (expected %d): %s\n", millis(), TAG, header[0],
                  FORMAT_VERSION, path.c_str());
    file.close();
    return entries;
  }

  const uint8_t count = header[1];
  for (uint8_t i = 0; i < count; i++) {
    uint8_t data[6];
    if (file.read(data, 6) != 6) {
      break;
    }
    BookmarkEntry entry;
    entry.bookPercent = data[0];
    entry.chapterPercent = data[1];
    entry.spineIndex = data[2] | (data[3] << 8);
    entry.pageIndex = data[4] | (data[5] << 8);
    entries.push_back(entry);
  }

  file.close();
  return entries;
}

bool BookmarkStore::writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries) {
  FsFile file;
  if (!SdMan.openFileForWrite(TAG, path, file)) {
    return false;
  }
  uint8_t header[2] = {FORMAT_VERSION, static_cast<uint8_t>(entries.size())};
  if (file.write(header, 2) != 2) {
    file.close();
    return false;
  }

  for (const auto& entry : entries) {
    uint8_t data[6];
    data[0] = entry.bookPercent;
    data[1] = entry.chapterPercent;
    data[2] = entry.spineIndex & 0xFF;
    data[3] = (entry.spineIndex >> 8) & 0xFF;
    data[4] = entry.pageIndex & 0xFF;
    data[5] = (entry.pageIndex >> 8) & 0xFF;
    if (file.write(data, 6) != 6) {
      file.close();
      return false;
    }
  }

  file.close();
  return true;
}

bool BookmarkStore::addBookmark(const std::string& bookPath, const BookmarkEntry& entry) {
  SdMan.mkdir(BOOKMARKS_DIR);
  const std::string path = getBookmarkPath(bookPath);

  auto entries = loadBookmarks(bookPath);

  // Skip duplicate (same exact position)
  if (std::any_of(entries.begin(), entries.end(), [&entry](const BookmarkEntry& existing) {
        return existing.spineIndex == entry.spineIndex && existing.pageIndex == entry.pageIndex;
      })) {
    Serial.printf("[%lu] [%s] Bookmark already exists at spine %d page %d\n", millis(), TAG, entry.spineIndex,
                  entry.pageIndex);
    return true;
  }

  entries.push_back(entry);

  // Sort by bookPercent ascending
  std::sort(entries.begin(), entries.end(),
            [](const BookmarkEntry& a, const BookmarkEntry& b) { return a.bookPercent < b.bookPercent; });

  // Enforce max 255 entries (uint8_t count)
  if (entries.size() > 255) {
    entries.resize(255);
  }

  const bool ok = writeBookmarks(path, entries);
  if (ok) {
    Serial.printf("[%lu] [%s] Bookmark added at %d%% (total: %d)\n", millis(), TAG, entry.bookPercent,
                  static_cast<int>(entries.size()));
  }
  return ok;
}

bool BookmarkStore::deleteBookmark(const std::string& bookPath, int index) {
  const std::string path = getBookmarkPath(bookPath);
  auto entries = loadBookmarks(bookPath);

  if (index < 0 || index >= static_cast<int>(entries.size())) {
    return false;
  }

  const int bookPercent = entries[index].bookPercent;
  entries.erase(entries.begin() + index);
  const bool ok = writeBookmarks(path, entries);
  if (ok) {
    Serial.printf("[%lu] [%s] Bookmark deleted at %d%% (remaining: %d)\n", millis(), TAG, bookPercent,
                  static_cast<int>(entries.size()));
  }
  return ok;
}
