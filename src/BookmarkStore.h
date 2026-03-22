#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  uint8_t bookPercent;        // 0-100 overall book progress
  uint16_t chapterPageCount;  // Total page count of the chapter at the time of bookmarking
  uint16_t chapterProgress;   // Number of pages into the chapter at the time of bookmarking
  uint16_t spineIndex;        // Spine item index
  uint16_t pageIndex;         // Page index within spine item
  std::string summary;        // First few words of a page to help identify it
};

// Stores and retrieves bookmarks in binary files on the SD card.
// Files are stored at /.crosspoint/bookmarks/<path-hash>.bookmarks
// Binary format: [version:1][count:1][entries: variable]
// Each entry: [bookPercent:1][chapterPageCount:2][chapterProgress:2][spineIndex:2 LE][pageIndex:2 LE][summaryLen:2
// LE][summary: summaryLen bytes]
class BookmarkStore {
 public:
  // Add a bookmark. Skips if a bookmark at the same position (spineIndex + pageIndex) already exists.
  // Returns true if the bookmark was added (or already existed).
  static bool addBookmark(const std::string& bookPath, const BookmarkEntry& entry);

  // Load all bookmarks for a book, sorted by bookPercent ascending.
  static std::vector<BookmarkEntry> loadBookmarks(const std::string& bookPath);

  // Delete a bookmark at the given index. Returns true on success.
  static bool deleteBookmark(const std::string& bookPath, int index);

  static const int LIMIT = 256;

 private:
  static std::string getBookmarkPath(const std::string& bookPath);
  static bool writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries);
  static constexpr uint8_t FORMAT_VERSION = 1;
  static constexpr const char* BOOKMARKS_DIR = "/.crosspoint/bookmarks";
  static constexpr const char* TAG = "BKM";
};