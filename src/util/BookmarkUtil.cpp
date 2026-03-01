#include "BookmarkUtil.h"
#include <vector>
#include <Logging.h>

void BookmarkUtil::load(int maxBookmarks) {
    bookmarks.clear();
    bookmarks.reserve(maxBookmarks);
    for (int i = 0; i < maxBookmarks; i++) {
      FsFile f;
      std::optional<Bookmark> newBookmark;
      if (Storage.openFileForRead("BKM", epub->getCachePath() + "/bookmark_" + std::to_string(i) + ".bin", f)) {
        uint8_t data[6];
        int dataSize = f.read(data, 6);
        if (dataSize == 4 || dataSize == 6) {
          int currentSpineIndex = data[0] + (data[1] << 8);
          int pageNumber = data[2] + (data[3] << 8);
          LOG_DBG("BKM", "Loaded bookmark: %d, %d", currentSpineIndex, pageNumber);
          newBookmark = Bookmark{currentSpineIndex, pageNumber, ""};
        }
        f.close();
      }
      bookmarks.push_back(std::move(newBookmark));
    }
}

std::optional<Bookmark> BookmarkUtil::getBookmark(int bookmarkIndex) {
  return bookmarks.at(bookmarkIndex);
}

void BookmarkUtil::deleteBookmark(int bookmarkIndex) {
  Storage.remove((epub->getCachePath() + "/bookmark_" + std::to_string(bookmarkIndex) + ".bin").c_str());
  bookmarks.at(bookmarkIndex) = std::nullopt;
}

Bookmark BookmarkUtil::saveBookmark(int bookmarkIndex, int currentSpineIndex, int currentPage) {
  FsFile f;
  if (Storage.openFileForWrite("BKM", epub->getCachePath() + "/bookmark_" + std::to_string(bookmarkIndex) + ".bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    LOG_DBG("BKM", "Bookmark saved: Chapter %d, Page %d", currentSpineIndex, currentPage);
  } else {
    LOG_ERR("BKM", "Could not save bookmark!");
  }

  // save in memory
  Bookmark newBookmark{currentSpineIndex, currentPage, ""};
  bookmarks.at(bookmarkIndex) = newBookmark;

  return newBookmark;
}

bool BookmarkUtil::doesBookmarkExist(int bookmarkIndex) {
  return bookmarks.at(bookmarkIndex).has_value();
}