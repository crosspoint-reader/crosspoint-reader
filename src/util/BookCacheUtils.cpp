#include "BookCacheUtils.h"

#include <BookCachePath.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

namespace {

constexpr char CACHE_ROOT[] = "/.crosspoint";

}  // namespace

bool isBookCacheDirectoryName(const char* name) { return name && BookCachePath::isCacheDirName(name); }

std::string bookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return BookCachePath::forBook(CACHE_ROOT, BookCachePath::EPUB_PREFIX, path);
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return BookCachePath::forBook(CACHE_ROOT, BookCachePath::XTC_PREFIX, path);
  }
  // Markdown is read by the TXT reader, so it caches under the same prefix.
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return BookCachePath::forBook(CACHE_ROOT, BookCachePath::TXT_PREFIX, path);
  }
  return {};
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, CACHE_ROOT).clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, CACHE_ROOT).clearCache();
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    Txt(path, CACHE_ROOT).clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool clearCacheAt(const std::string& path) {
  const std::string cachePath = bookCachePath(path);
  if (cachePath.empty() || !Storage.exists(cachePath.c_str())) {
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("BookCache", "Failed to clear cache dir %s", cachePath.c_str());
    return false;
  }
  return true;
}

BookCacheMove moveBookCache(const std::string& oldPath, const std::string& newPath) {
  const std::string newCachePath = bookCachePath(newPath);
  const std::string oldCachePath = bookCachePath(oldPath);
  if (oldCachePath.empty()) {
    return {};
  }

  if (newCachePath.empty()) {
    clearBookCache(oldPath);
    return {oldCachePath, "", true};
  }

  if (!Storage.exists(oldCachePath.c_str())) {
    return {oldCachePath, newCachePath, false};
  }

  if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
    LOG_ERR("BookCache", "Failed to move cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    return {oldCachePath, oldCachePath, false};
  }

  return {oldCachePath, newCachePath, false};
}
