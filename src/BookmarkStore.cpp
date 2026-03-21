#include "BookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

std::string BookmarkStore::getBookmarkPath(const std::string& bookPath) {
  // use string hash of the book to save bookmarks, same as is done for epub cache dirs
  const std::string epubKey = std::to_string(std::hash<std::string>{}(bookPath));
  return std::string(BOOKMARKS_DIR) + "/" + epubKey + ".bookmarks";
}

std::vector<BookmarkEntry> BookmarkStore::loadBookmarks(const std::string& bookPath) {
  std::vector<BookmarkEntry> entries;
  const std::string path = getBookmarkPath(bookPath);

  FsFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_DBG(TAG, "No bookmark file found");
    return entries;
  }

  uint8_t header[2];
  const int bytesRead = file.read(header, 2);
  if (bytesRead != 2) {
    file.close();
    return entries;
  }
  if (header[0] != FORMAT_VERSION) {
    LOG_DBG(TAG, "Skipping bookmark file with version %d (expected %d): %s", header[0],
                  FORMAT_VERSION, path.c_str());
    file.close();
    return entries;
  }

  const uint8_t count = header[1];
  for (uint8_t i = 0; i < count; i++) {
    uint8_t data[9];
    if (file.read(data, 9) != 9) {
      break;
    }
    BookmarkEntry entry;
    entry.bookPercent = data[0];
    entry.chapterPageCount = data[1] | (data[2] << 8);
    entry.chapterProgress = data[3] | (data[4] << 8);
    entry.spineIndex = data[5] | (data[6] << 8);
    entry.pageIndex = data[7] | (data[8] << 8);

    uint8_t lenData[2];
    if (file.read(lenData, 2) != 2) {
      break;
    }
    uint16_t summaryLen = lenData[0] | (lenData[1] << 8);
    std::vector<char> buf(summaryLen);
    if (file.read(buf.data(), summaryLen) != summaryLen) {
      break;
    }
    entry.summary = std::string(buf.begin(), buf.end());

    entries.push_back(entry);
  }

  file.close();
  return entries;
}

bool BookmarkStore::writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries) {
  if (entries.size() > 255) {
    LOG_DBG(TAG, "Cannot write bookmarks: too many entries (%d)", static_cast<int>(entries.size()));
    return false;
  }
  FsFile file;
  if (!Storage.openFileForWrite(TAG, path, file)) {
    LOG_DBG(TAG, "Cannot open bookmark file for writing: %s", path.c_str());
    return false;
  }
  uint8_t header[2] = {FORMAT_VERSION, static_cast<uint8_t>(entries.size())};
  if (file.write(header, 2) != 2) {
    file.close();
    return false;
  }

  for (const auto& entry : entries) {
    uint8_t data[9];
    data[0] = entry.bookPercent;
    data[1] = entry.chapterPageCount & 0xFF;
    data[2] = (entry.chapterPageCount >> 8) & 0xFF;
    data[3] = entry.chapterProgress & 0xFF;
    data[4] = (entry.chapterProgress >> 8) & 0xFF;
    data[5] = entry.spineIndex & 0xFF;
    data[6] = (entry.spineIndex >> 8) & 0xFF;
    data[7] = entry.pageIndex & 0xFF;
    data[8] = (entry.pageIndex >> 8) & 0xFF;
    if (file.write(data, 9) != 9) {
      file.close();
      return false;
    }

    // trim summary of double whitespaces and newlines, and trim whitespace from start and end
    std::string summary = entry.summary;
    summary.erase(std::unique(summary.begin(), summary.end(),
                              [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
                  summary.end());
    summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
    summary.erase(summary.begin(), std::find_if(summary.begin(), summary.end(),
                                                [](unsigned char ch) { return !std::isspace(ch); }));
    summary.erase(std::find_if(summary.rbegin(), summary.rend(),
                                                [](unsigned char ch) { return !std::isspace(ch); }).base(),
                  summary.end());

    // truncate summary to 48 characters before saving
    uint16_t len = std::min(entry.summary.size(), static_cast<size_t>(48));
    uint8_t lenData[2] = { static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>((len >> 8) & 0xFF) };
    if (file.write(lenData, 2) != 2) {
      file.close();
      return false;
    }
    if (file.write(entry.summary.data(), len) != len) {
      file.close();
      return false;
    }
  }

  file.close();
  return true;
}

bool BookmarkStore::addBookmark(const std::string& bookPath, const BookmarkEntry& entry) {
  Storage.mkdir(BOOKMARKS_DIR);
  const std::string path = getBookmarkPath(bookPath);

  auto entries = loadBookmarks(bookPath);

  // Skip duplicate (same exact position)
  if (std::any_of(entries.begin(), entries.end(), [&entry](const BookmarkEntry& existing) {
        return existing.spineIndex == entry.spineIndex && existing.pageIndex == entry.pageIndex;
      })) {
    LOG_DBG(TAG, "Bookmark already exists at spine %d page %d", entry.spineIndex,
                  entry.pageIndex);
    return true;
  }

  // Reject if at capacity (uint8_t count field supports max 256)
  if (entries.size() >= LIMIT) {
    LOG_DBG(TAG, "Bookmark limit reached (%d)", LIMIT);
    return false;
  }

  entries.insert(entries.begin(), entry);

  const bool ok = writeBookmarks(path, entries);
  if (ok) {
    LOG_DBG(TAG, "Bookmark added at %d%% (total: %d)", entry.bookPercent,
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
    LOG_DBG(TAG, "Bookmark deleted at %d%% (remaining: %d)", bookPercent,
                  static_cast<int>(entries.size()));
  }
  return ok;
}