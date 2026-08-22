#include "BookCacheUtils.h"

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

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

std::string bookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, CACHE_ROOT).getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, CACHE_ROOT).getCachePath();
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, CACHE_ROOT).getCachePath();
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

  if (Storage.exists(oldCachePath.c_str())) {
    // SdFat's rename does not overwrite, so a stale cache left at the
    // destination path would make the move fail.
    if (Storage.exists(newCachePath.c_str())) {
      Storage.removeDir(newCachePath.c_str());
    }
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
