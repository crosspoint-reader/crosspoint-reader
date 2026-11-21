#include "BookDataMove.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>
#include <common/FsUtf.h>
#include <common/upcase.h>

#include "BookCacheUtils.h"
#include "BookmarkUtil.h"
#include "RecentBooksStore.h"

namespace {

std::string bookCachePathFor(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  return "";
}

bool isFolder(const std::string& path) { return Storage.open(path.c_str()).isDirectory(); }

bool hasBookmarks(const std::string& path) { return FsHelpers::hasReflowableBookExtension(path); }

// Same match as SdFat's long-name lookup (FatFile::cmpName): UTF-16 units folded by toUpcase().
bool sameFatName(const std::string& a, const std::string& b) {
  const char *pa = a.c_str(), *endA = pa + a.size(), *pb = b.c_str(), *endB = pb + b.size();
  while (pa < endA && pb < endB) {
    uint16_t hiA, loA, hiB, loB;
    pa = FsUtf::mbToU16(pa, endA, &hiA, &loA);
    pb = FsUtf::mbToU16(pb, endB, &hiB, &loB);
    if (!pa || !pb || toUpcase(hiA) != toUpcase(hiB) || toUpcase(loA) != toUpcase(loB)) return false;
  }
  return pa == endA && pb == endB;
}

// FAT matches /Books_A.epub and /books/A.epub, accented letters included, to one bookmarks file.
bool shareBookmarks(const std::string& a, const std::string& b) {
  return hasBookmarks(a) && hasBookmarks(b) &&
         sameFatName(BookmarkUtil::getBookmarkPath(a), BookmarkUtil::getBookmarkPath(b));
}

}  // namespace

void clearBookData(const std::string& path, const std::string& sourcePath) {
  if (bookCachePathFor(path).empty() || isFolder(path)) return;
  clearBookCache(path);
  if (!hasBookmarks(path) || shareBookmarks(path, sourcePath)) return;
  const std::string bookmarkPath = BookmarkUtil::getBookmarkPath(path);
  if (Storage.exists(bookmarkPath.c_str()) && !Storage.remove(bookmarkPath.c_str())) {
    LOG_ERR("BDM", "Failed to remove bookmarks file %s (non-fatal)", bookmarkPath.c_str());
  }
}

void moveBookData(const std::string& oldPath, const std::string& newPath) {
  if (isFolder(newPath)) return;
  clearBookData(newPath, oldPath);
  const std::string oldCachePath = bookCachePathFor(oldPath);
  if (oldCachePath.empty()) return;
  const std::string newCachePath = bookCachePathFor(newPath);
  if (newCachePath.empty()) {
    clearBookCache(oldPath);
    RECENT_BOOKS.removeByPath(oldPath);
    return;
  }

  bool cacheMoved = true;
  if (Storage.exists(oldCachePath.c_str())) {
    cacheMoved = Storage.rename(oldCachePath.c_str(), newCachePath.c_str());
    if (cacheMoved) {
      // Rendered pages store absolute image paths under the old cache dir; they rebuild from progress.bin.
      Storage.removeDir((newCachePath + "/sections").c_str());
    } else {
      LOG_ERR("BDM", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  const std::string oldBookmarkPath = BookmarkUtil::getBookmarkPath(oldPath);
  const std::string newBookmarkPath = BookmarkUtil::getBookmarkPath(newPath);
  if (hasBookmarks(oldPath) && hasBookmarks(newPath) && !shareBookmarks(oldPath, newPath) &&
      Storage.exists(oldBookmarkPath.c_str())) {
    if (!Storage.rename(oldBookmarkPath.c_str(), newBookmarkPath.c_str())) {
      LOG_ERR("BDM", "Failed to rename bookmarks file %s -> %s (non-fatal)", oldBookmarkPath.c_str(),
              newBookmarkPath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, cacheMoved ? newCachePath : oldCachePath);
}
