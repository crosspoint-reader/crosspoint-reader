#pragma once
#include <Epub.h>

#include <vector>

struct BookmarkItem {
  int currentSpineIndex;
  int currentPage;
  int pageCount;
  std::string summary;
};

// Utility for managing bookmarks for a given epub
// We now keep bookmarks in-place rather than as raw pointers
// to avoid dangling pointer bugs. std::optional lets us represent
// an empty slot while preserving fixed indexing.
#include <optional>
class BookmarkUtil final {
  const int MAX_SUMMARY_LENGTH = 48;

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  std::vector<std::optional<BookmarkItem>> bookmarks;

 public:
  explicit BookmarkUtil(const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : epub(epub), epubPath(epubPath) {}

  void load(int maxBookmarks);
  std::optional<BookmarkItem> getBookmark(int bookmarkIndex);
  void deleteBookmark(int bookmarkIndex);
  // same as overriding
  BookmarkItem saveBookmark(int bookmarkIndex, int currentSpineIndex, int currentPage, int pageCount,
                            std::string summary);
  bool doesBookmarkExist(int bookmarkIndex);
};