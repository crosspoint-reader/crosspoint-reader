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
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return "";
}

}  // namespace

void moveBookData(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = bookCachePathFor(oldPath);
  const std::string newCachePath = bookCachePathFor(newPath);
  const std::string oldBookmarkPath = BookmarkUtil::getBookmarkPath(oldPath);
  const std::string newBookmarkPath = BookmarkUtil::getBookmarkPath(newPath);

  // BookmarkUtil::getBookmarkPath() flattens the full path into a filename, so two distinct
  // book paths can collide onto the same bookmarks file (e.g. a rename that only changes the
  // extension). When they collide, the bookmarks file is already at the right place: don't
  // remove it as "stale" destination data, and don't rename it onto itself.
  const bool bookmarkPathsCollide = oldBookmarkPath == newBookmarkPath;

  // Clear any stale cache/bookmarks already sitting at the destination.
  clearBookCache(newPath);
  if (!bookmarkPathsCollide && Storage.exists(newBookmarkPath.c_str()) && !Storage.remove(newBookmarkPath.c_str())) {
    LOG_ERR("BDM", "Failed to remove stale bookmarks file at %s (non-fatal)", newBookmarkPath.c_str());
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

  // newCachePath is empty when the destination isn't a supported book type (epub/xtc/txt).
  // updatePath() would otherwise rewrite coverBmpPath using an empty prefix, producing a
  // broken path, so drop the recent entry instead of repointing it.
  if (newCachePath.empty()) {
    RECENT_BOOKS.removeByPath(oldPath);
  } else {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  }
}
