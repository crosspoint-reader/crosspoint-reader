#include "BookmarkUtil.h"
#include <string>
#include <algorithm>

std::string BookmarkUtil::getBookmarksDir() {
    return "/.crosspoint/bookmarks/";
}

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
    const size_t lastSlash = bookPath.find_last_of('/');
    std::string bookName = (lastSlash != std::string::npos) ? bookPath.substr(lastSlash + 1) : bookPath;
    std::replace(bookName.begin(), bookName.end(), '/', '_');
    std::replace(bookName.begin(), bookName.end(), '\\', '_');
    const size_t lastDot = bookName.find_last_of('.');
    if (lastDot != std::string::npos) {
        bookName.erase(lastDot);
        bookName += ".json";
    } else {
        bookName += ".json";
    }
    return getBookmarksDir() + bookName;
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(
      std::unique(summary.begin(), summary.end(), [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
      summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  if (summary.size() > 72) {
    summary.resize(72);
  }
  return summary;
}
