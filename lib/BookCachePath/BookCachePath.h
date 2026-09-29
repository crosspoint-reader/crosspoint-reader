#pragma once

#include <string>
#include <string_view>

// Depends on neither Arduino nor storage, so the host tests can cover it.
namespace BookCachePath {

inline constexpr std::string_view EPUB_PREFIX = "epub_";
inline constexpr std::string_view XTC_PREFIX = "xtc_";
inline constexpr std::string_view TXT_PREFIX = "txt_";

// Keyed on the full path, so a book that moves maps to a different directory
// and its cache has to be moved along with it.
std::string forBook(std::string_view cacheRoot, std::string_view prefix, const std::string& bookPath);

bool isCacheDirName(std::string_view name);

}  // namespace BookCachePath
