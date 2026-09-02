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

bool moveBookData(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = bookCachePathFor(oldPath);
  const std::string newCachePath = bookCachePathFor(newPath);
  const std::string oldBookmarkPath = BookmarkUtil::getBookmarkPath(oldPath);
  const std::string newBookmarkPath = BookmarkUtil::getBookmarkPath(newPath);

  // Clear any stale cache/bookmarks already sitting at the destination.
  clearBookCache(newPath);
  if (Storage.exists(newBookmarkPath.c_str()) && !Storage.remove(newBookmarkPath.c_str())) {
    LOG_ERR("BDM", "Failed to remove stale bookmarks file at %s (non-fatal)", newBookmarkPath.c_str());
  }

  bool ok = true;

  if (!oldCachePath.empty() && !newCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BDM", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
      ok = false;
    }
  }

  if (Storage.exists(oldBookmarkPath.c_str())) {
    if (!Storage.rename(oldBookmarkPath.c_str(), newBookmarkPath.c_str())) {
      LOG_ERR("BDM", "Failed to rename bookmarks file %s -> %s (non-fatal)", oldBookmarkPath.c_str(),
              newBookmarkPath.c_str());
      ok = false;
    }
  }

  RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  return ok;
}
