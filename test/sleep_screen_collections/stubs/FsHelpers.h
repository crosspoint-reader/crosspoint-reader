#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace FsHelpers {

inline bool hasBmpExtension(const std::string_view name) {
  static constexpr char extension[] = ".bmp";
  if (name.size() < sizeof(extension) - 1) return false;
  const size_t offset = name.size() - (sizeof(extension) - 1);
  for (size_t i = 0; i < sizeof(extension) - 1; i++) {
    if (std::tolower(static_cast<unsigned char>(name[offset + i])) != extension[i]) return false;
  }
  return true;
}

inline bool naturalLess(const std::string& lhs, const std::string& rhs) {
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](const char a, const char b) {
    return std::tolower(static_cast<unsigned char>(a)) < std::tolower(static_cast<unsigned char>(b));
  });
}

}  // namespace FsHelpers
