#include "BookmarkUtil.h"

#include <Logging.h>

#include <vector>

void BookmarkUtil::load(int maxBookmarks) {
  bookmarks.clear();
  bookmarks.reserve(maxBookmarks);
  for (int i = 0; i < maxBookmarks; i++) {
    FsFile f;
    std::optional<BookmarkItem> newBookmark;
    if (Storage.openFileForRead("BKM", epub->getCachePath() + "/bookmark_" + std::to_string(i) + ".bin", f)) {
      uint8_t data[6];
      int dataSize = f.read(data, 6);
      if (dataSize == 4 || dataSize == 6) {
        int currentSpineIndex = data[0] + (data[1] << 8);
        int pageNumber = data[2] + (data[3] << 8);
        int pageCount = data[4] + (data[5] << 8);
        std::string summary;
        if (dataSize == 6) {
          char summaryBuffer[MAX_SUMMARY_LENGTH + 1] = {0};  // MAX_SUMMARY_LENGTH chars + null terminator
          int summarySize = f.read((uint8_t*)summaryBuffer, MAX_SUMMARY_LENGTH);
          if (summarySize > 0) {
            summary = std::string(summaryBuffer, summarySize);
            // Replace null characters with spaces to avoid display issues
            for (char& c : summary) {
              if (c == '\0') c = ' ';
            }
          }
        }

        LOG_DBG("BKM", "Loaded bookmark: %d, %d/%d with summary: %s", currentSpineIndex, pageNumber, pageCount,
                summary.c_str());
        newBookmark = BookmarkItem{currentSpineIndex, pageNumber, pageCount, summary};
      }
      f.close();
    }
    bookmarks.push_back(std::move(newBookmark));
  }
}

std::optional<BookmarkItem> BookmarkUtil::getBookmark(int bookmarkIndex) { return bookmarks.at(bookmarkIndex); }

void BookmarkUtil::deleteBookmark(int bookmarkIndex) {
  Storage.remove((epub->getCachePath() + "/bookmark_" + std::to_string(bookmarkIndex) + ".bin").c_str());
  bookmarks.at(bookmarkIndex) = std::nullopt;
}

BookmarkItem BookmarkUtil::saveBookmark(int bookmarkIndex, int currentSpineIndex, int currentPage, int pageCount,
                                        std::string summary) {
  FsFile f;
  if (Storage.openFileForWrite("BKM", epub->getCachePath() + "/bookmark_" + std::to_string(bookmarkIndex) + ".bin",
                               f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);

    if (!summary.empty()) {
      // trim summary of trailing whitespace before writing
      summary.erase(0, summary.find_first_not_of(' '));
      summary.erase(summary.find_last_not_of(' ') + 1);
      // remove new lines and tabs to avoid display issues
      for (char& c : summary) {
        if (c == '\n' || c == '\t') c = ' ';
      }

      // remove double spaces for display space efficiency
      std::string::size_type pos;
      while ((pos = summary.find("  ")) != std::string::npos) {
        summary.replace(pos, 2, " ");
      }

      // with max size of 40 chars
      if (summary.size() > MAX_SUMMARY_LENGTH) {
        summary = summary.substr(0, MAX_SUMMARY_LENGTH);
      }

      // Replace null characters with spaces to avoid display issues
      for (char& c : summary) {
        if (c == '\0') c = ' ';
      }
      f.write(summary.c_str(), summary.size());
    }
    f.close();

    LOG_DBG("BKM", "Bookmark saved: Chapter %d, Page %d with summary: %s", currentSpineIndex, currentPage,
            summary.c_str());
  } else {
    LOG_ERR("BKM", "Could not save bookmark!");
  }

  // save in memory
  BookmarkItem newBookmark{currentSpineIndex, currentPage, pageCount, summary};
  bookmarks.at(bookmarkIndex) = newBookmark;

  return newBookmark;
}

bool BookmarkUtil::doesBookmarkExist(int bookmarkIndex) { return bookmarks.at(bookmarkIndex).has_value(); }