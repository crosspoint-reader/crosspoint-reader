#include "BookDataMove.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include "BookCacheUtils.h"
#include "BookmarkUtil.h"
#include "RecentBooksStore.h"

namespace {

std::string bookCachePathFor(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasPlainTextBookExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return "";
}

}  // namespace

void clearBookData(const std::string& path) {
  clearBookCache(path);
  const std::string bookmarkPath = BookmarkUtil::getBookmarkPath(path);
  if (Storage.exists(bookmarkPath.c_str()) && !Storage.remove(bookmarkPath.c_str())) {
    LOG_ERR("BDM", "Failed to remove bookmarks file %s (non-fatal)", bookmarkPath.c_str());
  }
}

void moveBookData(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = bookCachePathFor(oldPath);
  const std::string newCachePath = bookCachePathFor(newPath);
  const std::string oldBookmarkPath = BookmarkUtil::getBookmarkPath(oldPath);
  const std::string newBookmarkPath = BookmarkUtil::getBookmarkPath(newPath);
  // getBookmarkPath() flattens the path, so e.g. an extension-only rename maps both paths to
  // the same bookmarks file, which must then be neither removed nor renamed onto itself.
  const bool bookmarkPathsCollide = oldBookmarkPath == newBookmarkPath;

  if (bookmarkPathsCollide) {
    clearBookCache(newPath);
  } else {
    clearBookData(newPath);
  }

  if (!oldCachePath.empty() && !newCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BDM", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  if (!bookmarkPathsCollide && Storage.exists(oldBookmarkPath.c_str())) {
    if (!Storage.rename(oldBookmarkPath.c_str(), newBookmarkPath.c_str())) {
      LOG_ERR("BDM", "Failed to rename bookmarks file %s -> %s (non-fatal)", oldBookmarkPath.c_str(),
              newBookmarkPath.c_str());
    }
  }

  // An unsupported destination type has no cache path; updatePath() would store a broken
  // coverBmpPath, so drop the entry instead.
  if (newCachePath.empty()) {
    RECENT_BOOKS.removeByPath(oldPath);
  } else {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  }
}
