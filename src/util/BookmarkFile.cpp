#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "BookmarkUtil.h"

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  bookmarks.clear();

  // Read/write go through PersistableStoreBase so the JSON parser and
  // serializer stay instantiated once, in PersistableStore.cpp.
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  JsonDocument doc;
  bool migrating = false;
  if (Storage.exists(path.c_str())) {
    // Exists but failed to read/parse -- a real corruption, not a missing
    // file. Don't guess by falling back to the legacy path: that could load
    // an unrelated book's bookmarks (see the migration note below) and
    // silently overwrite this file with the wrong data.
    if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) {
      return false;
    }
  } else {
    // One-time fallback: this book's bookmarks may still live under the old,
    // collision-prone sanitized-path scheme (see getLegacyBookmarkPath()).
    // Load from there if so, then re-save below under the new path so this
    // fallback is only paid once per book.
    const std::string legacyPath = BookmarkUtil::getLegacyBookmarkPath(bookPath);
    if (!PersistableStoreBase::readDocFromFile(legacyPath.c_str(), doc)) {
      return false;
    }
    migrating = true;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    if (!obj["vo"].isNull()) {
      bookmark.visibleTextOffset = obj["vo"] | static_cast<uint32_t>(0);
      bookmark.hasVisibleTextOffset = true;
    }
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());

  if (migrating) {
    LOG_INF("BKM", "Migrating legacy bookmark file to hash-based path");
    // Deliberately does not remove the legacy file: it may be shared with
    // another book that collided under the old scheme (see
    // getLegacyBookmarkPath()) and hasn't migrated yet. Leaving it in place
    // costs a small orphaned file but keeps that book's migration possible;
    // deleting it here would strand it after only the first book to load.
    if (!save(bookPath, bookmarks)) {
      LOG_ERR("BKM", "Failed to save migrated bookmarks");
    }
  }

  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.hasVisibleTextOffset) {
      obj["vo"] = bookmark.visibleTextOffset;
    }
  }

  // writeDocToFile ensures /.crosspoint; the bookmarks subdirectory is ours.
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}
