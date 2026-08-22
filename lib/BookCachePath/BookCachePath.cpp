#include "BookCachePath.h"

#include <functional>

namespace BookCachePath {

std::string forBook(std::string_view cacheRoot, std::string_view prefix, const std::string& bookPath) {
  // Hash std::string, not the view: the digits are part of existing on-disk
  // directory names, so the hashed type has to stay what they were built with.
  const std::string hash = std::to_string(std::hash<std::string>{}(bookPath));

  std::string path;
  path.reserve(cacheRoot.size() + 1 + prefix.size() + hash.size());
  path.append(cacheRoot);
  path.push_back('/');
  path.append(prefix);
  path.append(hash);
  return path;
}

namespace {

bool isPrefixedHash(std::string_view name, std::string_view prefix) {
  if (name.substr(0, prefix.size()) != prefix) {
    return false;
  }
  const std::string_view hash = name.substr(prefix.size());
  return !hash.empty() && hash.find_first_not_of("0123456789") == std::string_view::npos;
}

}  // namespace

bool isCacheDirName(std::string_view name) {
  return isPrefixedHash(name, EPUB_PREFIX) || isPrefixedHash(name, XTC_PREFIX) || isPrefixedHash(name, TXT_PREFIX);
}

}  // namespace BookCachePath
