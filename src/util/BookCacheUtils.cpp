#include "BookCacheUtils.h"

#include <BookCachePath.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointState.h"
#include "RecentBooksStore.h"

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

void moveBookCache(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = bookCachePath(oldPath);
  if (oldCachePath.empty()) {
    return;
  }

  const std::string newCachePath = bookCachePath(newPath);
  if (newCachePath.empty()) {
    clearBookCache(oldPath);
    RECENT_BOOKS.removeByPath(oldPath);
    return;
  }

  // Drop whatever sits at the destination even when the book has no cache to
  // move: left there, it would be picked up as this book's own cache.
  if (Storage.exists(newCachePath.c_str())) {
    Storage.removeDir(newCachePath.c_str());
  }

  if (Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookCache", "Failed to move cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }
}
