#include "BookMove.h"

#include <HalStorage.h>
#include <Logging.h>

#include "BookCacheUtils.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

bool moveBook(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = bookCachePath(oldPath);
  const std::string newCachePath = bookCachePath(newPath);

  // SdFat must not rename a path that still has an open file, so callers hand
  // over a path they have already closed.
  if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
    LOG_ERR("BookMove", "Failed to move %s -> %s", oldPath.c_str(), newPath.c_str());
    return false;
  }

  moveBookCache(oldPath, newPath);

  if (newCachePath.empty()) {
    RECENT_BOOKS.removeByPath(oldPath);
  } else {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  }

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }
  return true;
}
