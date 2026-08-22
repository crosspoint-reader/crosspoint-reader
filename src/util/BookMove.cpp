#include "BookMove.h"

#include <HalStorage.h>
#include <Logging.h>

#include "BookCacheUtils.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

bool moveBook(const std::string& oldPath, const std::string& newPath) {
  // SdFat must not rename a path that still has an open file, so callers hand
  // over a path they have already closed.
  if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
    LOG_ERR("BookMove", "Failed to move %s -> %s", oldPath.c_str(), newPath.c_str());
    return false;
  }

  const BookCacheMove cache = moveBookCache(oldPath, newPath);
  if (cache.dropped) {
    RECENT_BOOKS.removeByPath(oldPath);
  } else {
    RECENT_BOOKS.updatePath(oldPath, newPath, cache.from, cache.to);
  }

  if (APP_STATE.openEpubPath == oldPath) {
    // A destination that is not a book leaves nothing to resume into: the
    // reader would open the new path as an EPUB whatever it now holds.
    APP_STATE.openEpubPath = cache.dropped ? "" : newPath;
    APP_STATE.saveToFile();
  }
  return true;
}
