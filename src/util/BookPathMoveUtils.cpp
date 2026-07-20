#include "BookPathMoveUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "BookCacheUtils.h"
#include "BookmarkUtil.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "clippings/ClippingStore.h"

namespace {
constexpr char CACHE_ROOT[] = "/.crosspoint";

bool removeIfPresent(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.remove(path.c_str()); }

void logCleanupFailure(const char* kind, const std::string& path) {
  LOG_ERR("BookMove", "Could not remove %s state for %s", kind, path.c_str());
}
}  // namespace

BookPathMoveResult moveBookFilePreservingUserState(const std::string& sourcePath, const std::string& destinationPath) {
  if (!Storage.exists(sourcePath.c_str())) return BookPathMoveResult::SourceMissing;
  if (Storage.exists(destinationPath.c_str())) return BookPathMoveResult::DestinationExists;

  if (!FsHelpers::hasEpubExtension(sourcePath) || !FsHelpers::hasEpubExtension(destinationPath)) {
    if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) return BookPathMoveResult::StorageError;
    // Non-EPUB cache formats do not yet carry the CrossVi reader suite state.
    // Clear their old path-key only after the rename succeeds.
    clearBookCache(sourcePath);
    return BookPathMoveResult::Moved;
  }

  const std::string sourceCachePath = Epub(sourcePath, CACHE_ROOT).getCachePath();
  const std::string destinationCachePath = Epub(destinationPath, CACHE_ROOT).getCachePath();
  if (!prepareBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath)) {
    return BookPathMoveResult::StateUnavailable;
  }

  ClippingStore clippings;
  if (!clippings.isLoaded()) {
    const ClippingStore::LoadResult load = clippings.loadForBook(sourcePath, "", "");
    if (!clippings.isLoaded()) {
      LOG_ERR("BookMove", "Could not inspect clipping state before move (%u)", static_cast<unsigned>(load));
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StateUnavailable;
    }
  }
  const ClippingCodec::BookMetadata sourceBook = clippings.book();
  const ClippingStore::RekeyResult prepared =
      clippings.prepareRekeyForBook(destinationPath, sourceBook.title, sourceBook.author, sourceBook.bookType);
  if (prepared != ClippingStore::RekeyResult::Prepared && prepared != ClippingStore::RekeyResult::Unchanged) {
    cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
    return BookPathMoveResult::StateUnavailable;
  }

  if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) {
    clippings.cancelPreparedRekey();
    cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
    return BookPathMoveResult::StorageError;
  }

  const bool cachePublished =
      finalizeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
  if (!cachePublished) {
    LOG_ERR("BookMove", "Could not publish moved cache state");
    if (Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
      clippings.cancelPreparedRekey();
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StateUnavailable;
    }
    // The destination book is now authoritative. Keep staging for Reader
    // recovery and continue finalizing every independently verified state.
    LOG_ERR("BookMove", "Could not roll back book after cache publication failure");
  }

  const ClippingStore::RekeyResult finalized = prepared == ClippingStore::RekeyResult::Unchanged
                                                   ? ClippingStore::RekeyResult::Unchanged
                                                   : clippings.finalizePreparedRekey();
  if (finalized != ClippingStore::RekeyResult::Rekeyed && finalized != ClippingStore::RekeyResult::Unchanged) {
    LOG_ERR("BookMove", "Could not finalize moved clipping state (%u)", static_cast<unsigned>(finalized));
    if (Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
      if (cachePublished) {
        discardPublishedBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      } else {
        cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      }
      clippings.cancelPreparedRekey();
      return BookPathMoveResult::StateUnavailable;
    }
    LOG_ERR("BookMove", "Could not roll back book after clipping publication failure");
  }

  if (cachePublished) {
    if (!completeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath)) {
      LOG_ERR("BookMove", "Could not remove old bookmark duplicate after move");
    }
  }
  RECENT_BOOKS.updatePath(sourcePath, destinationPath, sourceCachePath, destinationCachePath);
  if (APP_STATE.openEpubPath == sourcePath) {
    APP_STATE.openEpubPath = destinationPath;
    APP_STATE.saveToFile();
  }
  return BookPathMoveResult::Moved;
}

void removeBookUserStateAfterDelete(const std::string& bookPath) {
  if (!FsHelpers::hasEpubExtension(bookPath)) {
    clearBookCache(bookPath);
    const std::string bookmarkPath = BookmarkUtil::getBookmarkPath(bookPath);
    if (!removeIfPresent(bookmarkPath)) logCleanupFailure("bookmark", bookmarkPath);
    return;
  }

  const std::string cachePath = Epub(bookPath, CACHE_ROOT).getCachePath();
  if (!resetBookCacheUserStateAfterReplacement(cachePath, bookPath)) {
    logCleanupFailure("cache transaction", bookPath);
  }
  // Deletion/replacement makes every canonical bookmark at this path stale.
  // A verified empty canonical also prevents an ambiguous legacy key from
  // resurfacing for a future book copied to the same path.
  if (!BookmarkUtil::writeEmptyCanonicalBookmark(bookPath)) {
    logCleanupFailure("bookmark tombstone", bookPath);
  }
  if (!ClippingStore::removeFilesForBook(bookPath)) logCleanupFailure("clipping", bookPath);
}

void resetBookUserStateAfterReplacement(const std::string& bookPath) { removeBookUserStateAfterDelete(bookPath); }
