#pragma once

#include <string>

namespace FsHelpers {
inline bool hasEpubExtension(const std::string& path) {
  return path.size() >= 5 && path.compare(path.size() - 5, 5, ".epub") == 0;
}
inline bool hasTxtExtension(const std::string& path) {
  return path.size() >= 4 && path.compare(path.size() - 4, 4, ".txt") == 0;
}
inline bool hasMarkdownExtension(const std::string& path) {
  return path.size() >= 3 && path.compare(path.size() - 3, 3, ".md") == 0;
}
inline bool hasXtcExtension(const std::string& path) {
  return (path.size() >= 4 && path.compare(path.size() - 4, 4, ".xtc") == 0) ||
         (path.size() >= 5 && path.compare(path.size() - 5, 5, ".xtch") == 0);
}
}  // namespace FsHelpers
