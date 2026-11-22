#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>

#include "BookmarkUtil.h"

namespace {
constexpr size_t MAX_FILE_BYTES = 50000;  // SDCardManager::readFile limit

void readSyncState(const JsonDocument& doc, BookmarkFile::SyncState& state) {
  state.account = doc["acct"] | "";
  for (JsonVariantConst item : doc["del"].as<JsonArrayConst>()) {
    uint64_t id;
    if (BookmarkUtil::parseSyncId(item | "", id)) state.deleted.push_back(id);
  }
}

struct BookmarkView {
  const std::vector<BookmarkEntry>& entries;
  const BookmarkEntry* prepend = nullptr;
  const BookmarkFile::RemoveMatch* remove = nullptr;

  bool keeps(const BookmarkEntry& entry) const { return !remove || !(*remove)(entry); }
};

bool write(const std::string& bookPath, const BookmarkView& bookmarks, const BookmarkFile::SyncState& state);
}  // namespace

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks, SyncState* state) {
  bookmarks.clear();
  if (state) *state = {};

  // Read/write go through PersistableStoreBase so the JSON parser and
  // serializer stay instantiated once, in PersistableStore.cpp.
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) {
    return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.name = obj["name"] | "";
    if (bookmark.name.size() > BookmarkEntry::MAX_NAME_LENGTH) {
      bookmark.name.resize(BookmarkEntry::MAX_NAME_LENGTH);
    }
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    if (!obj["vo"].isNull()) {
      bookmark.visibleTextOffset = obj["vo"] | static_cast<uint32_t>(0);
      bookmark.hasVisibleTextOffset = true;
    }
    bookmark.synced = obj["sy"] | false;
  }
  if (state) readSyncState(doc, *state);

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks,
                        const BookmarkEntry* prepend, const RemoveMatch& remove) {
  const BookmarkView proposed{bookmarks, prepend, remove ? &remove : nullptr};
  // The lists callers hold do not carry deletions, so recover them from the file being replaced.
  SyncState state;
  {
    JsonDocument previous;
    const auto path = BookmarkUtil::getBookmarkPath(bookPath);
    if (PersistableStoreBase::readDocFromFile(path.c_str(), previous)) {
      readSyncState(previous, state);
      for (JsonObjectConst obj : previous["bookmarks"].as<JsonArrayConst>()) {
        // After a first sync every removal is recorded: a renamed bookmark loses its synced
        // flag while the server still holds it, and deleting an unknown id is harmless there.
        const char* xpath = obj["xpath"] | "";
        if (state.account.empty() || !xpath[0]) continue;
        const bool kept = (prepend && prepend->xpath == xpath) ||
                          std::any_of(bookmarks.begin(), bookmarks.end(), [&](const BookmarkEntry& bookmark) {
                            return bookmark.xpath == xpath && proposed.keeps(bookmark);
                          });
        if (!kept) state.deleted.push_back(BookmarkUtil::syncId(xpath));
      }
    } else if ((proposed.prepend || proposed.remove) && Storage.exists(path.c_str())) {
      LOG_ERR("BKM", "Cannot read existing bookmarks; toggle not saved");
      return false;
    }
  }
  return write(bookPath, proposed, state);
}

bool BookmarkFile::saveSynced(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks,
                              const std::string& account) {
  return write(bookPath, {bookmarks}, {{}, account});
}

namespace {
bool write(const std::string& bookPath, const BookmarkView& bookmarks, const BookmarkFile::SyncState& state) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  const auto append = [&](const BookmarkEntry& bookmark) {
    if (bookmark.name.size() > BookmarkEntry::MAX_NAME_LENGTH) {
      LOG_ERR("BKM", "Bookmark name exceeds %zu bytes", BookmarkEntry::MAX_NAME_LENGTH);
      return false;
    }
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    if (!bookmark.name.empty()) {
      obj["name"] = bookmark.name;
    }
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.hasVisibleTextOffset) {
      obj["vo"] = bookmark.visibleTextOffset;
    }
    if (bookmark.synced) {
      obj["sy"] = true;
    }
    return true;
  };
  if (bookmarks.prepend && !append(*bookmarks.prepend)) return false;
  for (const auto& bookmark : bookmarks.entries) {
    if (bookmarks.keeps(bookmark) && !append(bookmark)) return false;
  }
  LOG_DBG("BKM", "Saving %zu bookmarks to file", arr.size());
  if (!state.account.empty()) {
    doc["acct"] = state.account;
  }
  if (!state.deleted.empty()) {
    JsonArray del = doc["del"].to<JsonArray>();
    for (const uint64_t id : state.deleted) {
      char hex[BookmarkUtil::SYNC_ID_HEX_SIZE];
      BookmarkUtil::formatSyncId(id, hex);
      del.add(static_cast<const char*>(hex));  // const char* makes ArduinoJson copy the text
    }
  }

  // An incomplete or oversized document must not replace the file: the storage
  // layer cannot read back more than 50,000 bytes.
  if (doc.overflowed() || measureJson(doc) > MAX_FILE_BYTES) {
    LOG_ERR("BKM", "Bookmark file too large or out of memory; not saved");
    return false;
  }

  // writeDocToFile ensures /.crosspoint; the bookmarks subdirectory is ours.
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}
}  // namespace
