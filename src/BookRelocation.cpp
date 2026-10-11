#include "BookRelocation.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <vector>

#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"

namespace BookRelocation {
namespace {

constexpr char CACHE_ROOT[] = "/.crosspoint";
// A long file name is up to 255 UTF-16 units, so up to 765 bytes of UTF-8.
constexpr size_t NAME_BUFFER_BYTES = 768;

// "epub", "xtc" or "txt": a cache made by one reader is no use to another.
std::string cacheKind(const std::string& cacheDir) {
  const size_t slash = cacheDir.rfind('/');
  const size_t underscore = cacheDir.rfind('_');
  if (slash == std::string::npos || underscore == std::string::npos || underscore < slash) return {};
  return cacheDir.substr(slash + 1, underscore - slash - 1);
}

}  // namespace

std::string cacheDirFor(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return Epub(path, CACHE_ROOT).getCachePath();
  if (FsHelpers::hasXtcExtension(path)) return Xtc(path, CACHE_ROOT).getCachePath();
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Txt(path, CACHE_ROOT).getCachePath();
  }
  return {};
}

void bookMoved(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCache = cacheDirFor(oldPath);
  if (oldCache.empty()) return;
  const std::string newCache = cacheDirFor(newPath);

  if (newCache.empty() || cacheKind(newCache) != cacheKind(oldCache)) {
    // A new extension makes it another kind of book: nothing cached carries over.
    if (Storage.exists(oldCache.c_str())) Storage.removeDir(oldCache.c_str());
    RECENT_BOOKS.removeBook(oldPath);
    GLOBAL_BOOKMARKS.removeBySourcePath(oldPath);
    LOG_DBG("BKR", "%s became another kind of file; its cache is dropped", oldPath.c_str());
    return;
  }

  if (Storage.exists(oldCache.c_str())) {
    // Whatever sits under the new path's key belonged to a book deleted from that path.
    if (Storage.exists(newCache.c_str())) Storage.removeDir(newCache.c_str());
    if (!Storage.rename(oldCache.c_str(), newCache.c_str())) {
      LOG_ERR("BKR", "Could not move %s to %s; %s starts afresh", oldCache.c_str(), newCache.c_str(), newPath.c_str());
    }
  }

  RECENT_BOOKS.relocate(oldPath, newPath, oldCache, newCache);
  GLOBAL_BOOKMARKS.relocate(oldPath, newPath, newCache);
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }

  const std::string oldDocId = KOReaderDocumentId::calculateFromFilename(oldPath);
  const std::string newDocId = KOReaderDocumentId::calculateFromFilename(newPath);
  if (oldDocId != newDocId) {
    // NotFound is the common case: a book never read for long enough has no history.
    const auto result = READING_STATS.renameBook(oldDocId, newDocId);
    if (result == ReadingStatsStore::WriteResult::Failed || result == ReadingStatsStore::WriteResult::NoMemory) {
      LOG_ERR("BKR", "Reading stats for %s not carried over to %s", oldPath.c_str(), newPath.c_str());
    }
  }
  LOG_DBG("BKR", "Moved %s -> %s", oldPath.c_str(), newPath.c_str());
}

void folderMoved(const std::string& oldDir, const std::string& newDir) {
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_BYTES);
  if (!name) {
    LOG_ERR("BKR", "OOM: books under %s keep no state", newDir.c_str());
    return;
  }
  // Iterative, not recursive: a deep tree must not cost a stack frame and an open folder per level.
  std::vector<std::string> pending{newDir};
  while (!pending.empty()) {
    const std::string dirPath = std::move(pending.back());
    pending.pop_back();
    FsFile dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      HalSystem::feedWatchdog();
      entry.getName(name.get(), NAME_BUFFER_BYTES);
      const bool isDir = entry.isDirectory();
      entry.close();
      const std::string child = dirPath + "/" + name.get();
      if (isDir) {
        pending.push_back(child);
      } else {
        bookMoved(oldDir + child.substr(newDir.size()), child);
      }
    }
    dir.close();
  }
}

void moved(const std::string& oldPath, const std::string& newPath) {
  FsFile target = Storage.open(newPath.c_str());
  const bool isDir = target && target.isDirectory();
  if (target) target.close();
  if (isDir) {
    folderMoved(oldPath, newPath);
  } else {
    bookMoved(oldPath, newPath);
  }
}

}  // namespace BookRelocation
