#pragma once

#include <string>
#include <string_view>

namespace FsHelpers {
inline std::string normalisePath(const std::string& path) { return path; }
inline std::string decodeUriEscapes(const std::string& path) { return path; }
inline bool hasCssExtension(const std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".css";
}
inline bool hasPngExtension(const std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".png";
}
inline bool hasJpgExtension(const std::string_view path) {
  return (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") ||
         (path.size() >= 5 && path.substr(path.size() - 5) == ".jpeg");
}
}  // namespace FsHelpers
