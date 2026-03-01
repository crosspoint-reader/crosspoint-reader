#pragma once
#include <Epub.h>
#include <vector>

struct Bookmark {
  int currentSpineIndex;
  int currentPage;
  std::string summary;
};

// Utility for managing bookmarks for a given epub
class BookmarkUtil final {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  std::vector<Bookmark*> bookmarks;

 public:
  explicit BookmarkUtil(const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : epub(epub),
        epubPath(epubPath) {}

  void load(int maxBookmarks);
  Bookmark* getBookmark(int bookmarkIndex);
  void deleteBookmark(int bookmarkIndex);
  // same as overriding
  Bookmark saveBookmark(int bookmarkIndex, int currentSpineIndex, int currentPage);
  bool doesBookmarkExist(int bookmarkIndex);

};