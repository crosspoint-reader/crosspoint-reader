#include "BookmarkStore.h"

#include <SDCardManager.h>

#include <algorithm>

std::string BookmarkStore::getBookmarkPath(const std::string& bookPath) {
  std::string filename;
  const auto lastSlash = bookPath.rfind('/');
  if (lastSlash != std::string::npos) {
    filename = bookPath.substr(lastSlash + 1);
  } else {
    filename = bookPath;
  }
  const auto lastDot = filename.rfind('.');
  if (lastDot != std::string::npos) {
    filename.resize(lastDot);
  }
  if (filename.empty()) {
    filename = "untitled";
  }
  return std::string(BOOKMARKS_DIR) + "/" + filename + ".bookmarks";
}

std::vector<BookmarkEntry> BookmarkStore::loadBookmarks(const std::string& bookPath) {
  std::vector<BookmarkEntry> entries;
  const std::string path = getBookmarkPath(bookPath);

  FsFile file;
  if (!SdMan.openFileForRead(TAG, path, file)) {
    return entries;
  }

  uint8_t header[2];
  if (file.read(header, 2) != 2 || header[0] != FORMAT_VERSION) {
    file.close();
    return entries;
  }

  const uint8_t count = header[1];
  for (uint8_t i = 0; i < count; i++) {
    uint8_t data[5];
    if (file.read(data, 5) != 5) {
      break;
    }
    BookmarkEntry entry;
    entry.bookPercent = data[0];
    entry.spineIndex = data[1] | (data[2] << 8);
    entry.pageIndex = data[3] | (data[4] << 8);
    entries.push_back(entry);
  }

  file.close();
  return entries;
}

bool BookmarkStore::writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries) {
  FsFile file = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open bookmark file: %s\n", millis(), TAG, path.c_str());
    return false;
  }

  uint8_t header[2] = {FORMAT_VERSION, static_cast<uint8_t>(entries.size())};
  if (file.write(header, 2) != 2) {
    file.close();
    return false;
  }

  for (const auto& entry : entries) {
    uint8_t data[5];
    data[0] = entry.bookPercent;
    data[1] = entry.spineIndex & 0xFF;
    data[2] = (entry.spineIndex >> 8) & 0xFF;
    data[3] = entry.pageIndex & 0xFF;
    data[4] = (entry.pageIndex >> 8) & 0xFF;
    if (file.write(data, 5) != 5) {
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

  // Skip duplicate (same bookPercent)
  for (const auto& existing : entries) {
    if (existing.bookPercent == entry.bookPercent) {
      Serial.printf("[%lu] [%s] Bookmark at %d%% already exists\n", millis(), TAG, entry.bookPercent);
      return true;
    }
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

  entries.erase(entries.begin() + index);
  return writeBookmarks(path, entries);
}
