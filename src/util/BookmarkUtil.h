#pragma once
#include <string>

class BookmarkUtil {
 public:
  static std::string getBookmarksDir();
  static std::string getBookmarkPath(const std::string& bookPath);
  static std::string sanitizeBookmarkSummary(std::string summary);
};
