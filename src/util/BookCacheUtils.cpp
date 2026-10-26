#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Xtc.h>

#include <functional>

#include "BookmarkUtil.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {

std::string getBookCachePath(const std::string& path) {
  const char* prefix = nullptr;
  if (FsHelpers::hasReflowableBookExtension(path)) {
    prefix = "epub_";
  } else if (FsHelpers::hasXtcExtension(path)) {
    prefix = "xtc_";
  } else {
    return "";
  }
  return std::string("/.crosspoint/") + prefix + std::to_string(std::hash<std::string>{}(path));
}

bool moveStatePath(const std::string& oldPath, const std::string& newPath, bool& moved) {
  moved = false;
  if (oldPath.empty() || !Storage.exists(oldPath.c_str())) return true;
  if (Storage.exists(newPath.c_str())) {
    LOG_ERR("BookCache", "Move state target already exists: %s", newPath.c_str());
    return false;
  }
  moved = Storage.rename(oldPath.c_str(), newPath.c_str());
  if (!moved) LOG_ERR("BookCache", "Failed to move book state: %s -> %s", oldPath.c_str(), newPath.c_str());
  return moved;
}

void rollBackStatePath(const std::string& oldPath, const std::string& newPath, const bool moved) {
  if (moved && !Storage.rename(newPath.c_str(), oldPath.c_str())) {
    LOG_ERR("BookCache", "Failed to roll back book state: %s -> %s", newPath.c_str(), oldPath.c_str());
  }
}

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

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  return ClippingStore::deleteForFilePath(path, "epub");
}

bool moveBookFile(const std::string& from, const std::string& to) {
  const std::string oldCachePath = getBookCachePath(from);
  const std::string newCachePath = getBookCachePath(to);
  const bool hasBookmarks = FsHelpers::hasReflowableBookExtension(from);
  const std::string oldBookmarkPath = hasBookmarks ? BookmarkUtil::getBookmarkPath(from) : "";
  const std::string newBookmarkPath = hasBookmarks ? BookmarkUtil::getBookmarkPath(to) : "";
  bool cacheMoved = false;
  bool bookmarksMoved = false;
  if (!moveStatePath(oldCachePath, newCachePath, cacheMoved)) return false;
  if (!moveStatePath(oldBookmarkPath, newBookmarkPath, bookmarksMoved)) {
    rollBackStatePath(oldCachePath, newCachePath, cacheMoved);
    return false;
  }
  if (!ClippingStore::moveBook(from, to)) {
    rollBackStatePath(oldBookmarkPath, newBookmarkPath, bookmarksMoved);
    rollBackStatePath(oldCachePath, newCachePath, cacheMoved);
    return false;
  }

  RECENT_BOOKS.updatePath(from, to, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == from) {
    APP_STATE.openEpubPath = to;
    if (!APP_STATE.saveToFile()) LOG_ERR("BookCache", "Failed to save the moved open-book path");
  }
  return true;
}
