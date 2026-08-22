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

bool isCacheDirName(std::string_view name) {
  return name.substr(0, EPUB_PREFIX.size()) == EPUB_PREFIX || name.substr(0, XTC_PREFIX.size()) == XTC_PREFIX ||
         name.substr(0, TXT_PREFIX.size()) == TXT_PREFIX;
}

}  // namespace BookCachePath
