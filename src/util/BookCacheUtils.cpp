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
  if (FsHelpers::hasTxtExtension(path)) {
    return BookCachePath::forBook(CACHE_ROOT, BookCachePath::TXT_PREFIX, path);
  }
  return {};
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, CACHE_ROOT).clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, CACHE_ROOT).clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, CACHE_ROOT).clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

BookCacheMove moveBookCache(const std::string& oldPath, const std::string& newPath) {
  const std::string newCachePath = bookCachePath(newPath);

  // Anything already at the destination belongs to a book that no longer lives
  // there. Left in place it would be picked up as the moved file's own cache,
  // whether or not that file brings a cache of its own.
  if (!newCachePath.empty() && Storage.exists(newCachePath.c_str())) {
    if (!Storage.removeDir(newCachePath.c_str())) {
      LOG_ERR("BookCache", "Failed to clear cache dir %s (non-fatal)", newCachePath.c_str());
    }
  }

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
