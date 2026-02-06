#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  uint8_t bookPercent;     // 0-100 overall book progress
  uint8_t chapterPercent;  // 0-100 chapter progress
  uint16_t spineIndex;     // Spine item index
  uint16_t pageIndex;      // Page index within spine item
};

// Stores and retrieves bookmarks in binary files on the SD card.
// Files are stored at /.crosspoint/bookmarks/<book-filename>.bookmarks
// Binary format: [version:1][count:1][entries: count * 6 bytes]
// Each entry: [bookPercent:1][chapterPercent:1][spineIndex:2 LE][pageIndex:2 LE]
class BookmarkStore {
 public:
  // Add a bookmark. Skips if a bookmark at the same position (spineIndex + pageIndex) already exists.
  // Returns true if the bookmark was added (or already existed).
  static bool addBookmark(const std::string& bookPath, const BookmarkEntry& entry);

  // Load all bookmarks for a book, sorted by bookPercent ascending.
  static std::vector<BookmarkEntry> loadBookmarks(const std::string& bookPath);

  // Delete a bookmark at the given index. Returns true on success.
  static bool deleteBookmark(const std::string& bookPath, int index);

 private:
  static std::string getBookmarkPath(const std::string& bookPath);
  static bool writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries);
  static constexpr uint8_t FORMAT_VERSION = 2;
  static constexpr const char* BOOKMARKS_DIR = "/.crosspoint/bookmarks";
  static constexpr const char* TAG = "BKM";
};
