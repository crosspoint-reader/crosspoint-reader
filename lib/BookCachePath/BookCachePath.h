#pragma once

#include <string>
#include <string_view>

// Maps a book file to the directory holding its cache (metadata, rendered
// sections, reading progress). Pure string logic, kept free of Arduino and
// storage dependencies so it can be unit tested on the host.
namespace BookCachePath {

// One prefix per book type. Directory names under the cache root start with
// one of these, which is how a cache directory is told apart from other data.
inline constexpr std::string_view EPUB_PREFIX = "epub_";
inline constexpr std::string_view XTC_PREFIX = "xtc_";
inline constexpr std::string_view TXT_PREFIX = "txt_";

// Builds <cacheRoot>/<prefix><hash of bookPath>. Keyed on the full path, so a
// book that moves maps to a different directory and its cache has to follow.
std::string forBook(std::string_view cacheRoot, std::string_view prefix, const std::string& bookPath);

// True if a directory name directly under the cache root holds a book cache.
bool isCacheDirName(std::string_view name);

}  // namespace BookCachePath
