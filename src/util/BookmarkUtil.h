#pragma once
#include <string>

class BookmarkUtil {
 public:
  static std::string getBookmarksDir();
  // Hash-based, not the book's (sanitized) path: two different bookPaths
  // whose only difference is a '/' vs. a literal '_' flatten to the same
  // string under substitution (e.g. "/books/a/b.epub" and "/books/a_b.epub"
  // both become "books_a_b.json"), silently sharing -- and overwriting --
  // one bookmark file between two unrelated books. See getLegacyBookmarkPath().
  static std::string getBookmarkPath(const std::string& bookPath);
  // The old sanitized-path scheme getBookmarkPath() used before the
  // collision above was found. Kept only so BookmarkFile::load() can do a
  // one-time migration for files that still exist under it -- never write
  // here.
  static std::string getLegacyBookmarkPath(const std::string& bookPath);
  static std::string sanitizeBookmarkSummary(std::string summary);
};
