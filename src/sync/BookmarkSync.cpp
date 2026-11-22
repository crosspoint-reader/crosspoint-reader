#include "BookmarkSync.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "SyncRequest.h"
#include "util/BookmarkFile.h"
#include "util/BookmarkUtil.h"

namespace {
// Ten of the largest items the server stores (512-byte XPath, 256-character summary) fit the buffer.
constexpr size_t PAGE_SIZE = 10;
constexpr size_t MAX_XPATH_LENGTH = 512;  // server limit
// Bound what a remote list can add: the reader keeps the list in RAM, and the
// bookmark file must stay under the 50,000 bytes the storage layer reads back.
constexpr size_t MAX_BOOKMARKS = 128;
// Leaves room for every bookmark to receive a full-length name from the server later.
constexpr size_t MAX_LIST_BYTES = 24 * 1024;

// Approximate size of one bookmark in the bookmark file.
size_t storedBytes(const BookmarkEntry& bookmark) {
  return bookmark.xpath.size() + bookmark.summary.size() + bookmark.name.size() + 120;
}

bool uploadable(const BookmarkEntry& bookmark) {
  return !bookmark.synced && !bookmark.xpath.empty() && bookmark.xpath.size() <= MAX_XPATH_LENGTH;
}

bool upload(SyncRequest& sync, const std::string& url, JsonDocument& doc, const size_t count) {
  if (!sync.send(url, doc, true)) return false;
  if (doc["accepted"].as<size_t>() != count) {
    LOG_ERR("BKS", "Server did not acknowledge bookmarks");
    return false;
  }
  return true;
}

void setId(JsonObject item, const uint64_t id) {
  char hex[BookmarkUtil::SYNC_ID_HEX_SIZE];
  BookmarkUtil::formatSyncId(id, hex);
  item["id"] = static_cast<const char*>(hex);  // const char* makes ArduinoJson copy the text
}

bool pushDeletions(SyncRequest& sync, const std::string& url, JsonDocument& doc, const std::vector<uint64_t>& deleted) {
  for (size_t next = 0; next < deleted.size();) {
    doc.clear();
    JsonArray items = doc["items"].to<JsonArray>();
    const size_t end = std::min(deleted.size(), next + PAGE_SIZE);
    const size_t count = end - next;
    for (; next < end; ++next) {
      JsonObject item = items.add<JsonObject>();
      setId(item, deleted[next]);
      item["deleted"] = 1;
    }
    if (!upload(sync, url, doc, count)) return false;
  }
  return true;
}

bool pushNew(SyncRequest& sync, const std::string& url, JsonDocument& doc, std::vector<BookmarkEntry>& bookmarks,
             bool& changed) {
  for (size_t next = 0; next < bookmarks.size();) {
    doc.clear();
    JsonArray items = doc["items"].to<JsonArray>();
    const size_t first = next;
    size_t count = 0;
    for (; next < bookmarks.size() && count < PAGE_SIZE; ++next) {
      const BookmarkEntry& bookmark = bookmarks[next];
      if (!uploadable(bookmark)) continue;
      JsonObject item = items.add<JsonObject>();
      setId(item, BookmarkUtil::syncId(bookmark.xpath));
      item["xpath"] = bookmark.xpath;
      item["percentage"] = std::isfinite(bookmark.percentage) ? std::clamp(bookmark.percentage, 0.0f, 1.0f) : 0.0f;
      item["summary"] = BookmarkUtil::sanitizeBookmarkSummary(bookmark.summary);
      item["si"] = bookmark.computedSpineIndex;
      item["pc"] = bookmark.computedChapterPageCount;
      item["pp"] = bookmark.computedChapterProgress;
      item["name"] = bookmark.name;  // always sent: an empty name clears the server's label
      if (bookmark.hasVisibleTextOffset) item["vo"] = bookmark.visibleTextOffset;
      ++count;
    }
    if (count == 0) break;
    if (!upload(sync, url, doc, count)) return false;
    for (size_t i = first; i < next; ++i) {
      if (uploadable(bookmarks[i])) bookmarks[i].synced = true;
    }
    changed = true;
  }
  return true;
}

// Applies one server item. ids mirrors bookmarks so a deletion, which carries no XPath, can be matched.
void applyRemote(JsonObjectConst item, std::vector<BookmarkEntry>& bookmarks, std::vector<uint64_t>& ids,
                 bool& changed) {
  uint64_t id;
  if (!BookmarkUtil::parseSyncId(item["id"] | "", id)) return;  // not created by a CrossPoint device
  const auto match = std::find(ids.begin(), ids.end(), id);
  const auto index = match - ids.begin();

  if (item["deleted"].as<unsigned>() != 0) {
    if (match == ids.end()) return;
    bookmarks.erase(bookmarks.begin() + index);
    ids.erase(match);
    changed = true;
    return;
  }
  // A server that predates labels sends no name; local names are then left alone.
  const char* name = item["name"] | static_cast<const char*>(nullptr);
  if (name && strlen(name) > BookmarkEntry::MAX_NAME_LENGTH) name = nullptr;
  const bool hasOffset = item["vo"].is<uint32_t>();

  if (match != ids.end()) {
    // Local changes were uploaded before this scan, so the server's copy is the newer one.
    BookmarkEntry& local = bookmarks[index];
    if (!local.synced) {
      local.synced = true;
      changed = true;
    }
    if (name && local.name != name) {
      local.name = name;
      changed = true;
    }
    if (hasOffset && !local.hasVisibleTextOffset) {
      local.visibleTextOffset = item["vo"];
      local.hasVisibleTextOffset = true;
      changed = true;
    }
    return;
  }

  const char* xpath = item["xpath"] | "";
  // An item whose id does not come from its XPath could never be deleted from this device.
  if (!item["percentage"].is<float>() || !xpath[0] || strlen(xpath) > MAX_XPATH_LENGTH ||
      BookmarkUtil::syncId(xpath) != id) {
    return;
  }
  size_t listBytes = 0;
  for (const BookmarkEntry& bookmark : bookmarks) listBytes += storedBytes(bookmark);
  if (bookmarks.size() >= MAX_BOOKMARKS || listBytes + strlen(xpath) + 400 > MAX_LIST_BYTES) {
    LOG_ERR("BKS", "Bookmark limit reached; remote bookmark skipped");
    return;
  }
  BookmarkEntry entry;
  entry.xpath = xpath;
  entry.percentage = std::clamp(item["percentage"].as<float>(), 0.0f, 1.0f);
  entry.summary = BookmarkUtil::sanitizeBookmarkSummary(item["summary"] | "");
  entry.computedSpineIndex = item["si"] | static_cast<uint16_t>(0);
  entry.computedChapterPageCount = item["pc"] | static_cast<uint16_t>(0);
  entry.computedChapterProgress = item["pp"] | static_cast<uint16_t>(0);
  if (name) entry.name = name;
  if (hasOffset) {
    entry.visibleTextOffset = item["vo"];
    entry.hasVisibleTextOffset = true;
  }
  entry.synced = true;
  // The server lists oldest first and the reader keeps newest first.
  bookmarks.insert(bookmarks.begin(), std::move(entry));
  ids.insert(ids.begin(), id);
  changed = true;
}
}  // namespace

bool bookmarkSync::run(SyncRequest& sync, const std::string& bookPath, const std::string& documentHash) {
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/bookmarks/" + documentHash;
  // The document id is part of the scope: changing the match method points the book at another server document.
  const std::string account = SyncRequest::account() + "#" + documentHash;
  JsonDocument doc;

  std::vector<BookmarkEntry> bookmarks;
  BookmarkFile::SyncState state;
  if (!BookmarkFile::load(bookPath, bookmarks, &state) &&
      Storage.exists(BookmarkUtil::getBookmarkPath(bookPath).c_str())) {
    // An unreadable file is not an empty list: saving the server's list over it would drop local bookmarks.
    LOG_ERR("BKS", "Bookmark file unreadable; sync skipped");
    return false;
  }
  bool changed = !state.deleted.empty();
  if (state.account != account) {
    // The flags describe another account or document: this one must receive every bookmark and none of the deletions.
    for (BookmarkEntry& bookmark : bookmarks) bookmark.synced = false;
    state.deleted.clear();
    changed = !bookmarks.empty();
  }

  // Deletions go first so a bookmark removed and added again ends up present.
  if (!pushDeletions(sync, url, doc, state.deleted)) return false;
  // Saved now so a failed upload below does not send acknowledged deletions again.
  if (changed && !BookmarkFile::saveSynced(bookPath, bookmarks, account)) return false;
  changed = false;
  if (!pushNew(sync, url, doc, bookmarks, changed)) return false;
  if (changed && !BookmarkFile::saveSynced(bookPath, bookmarks, account)) return false;
  changed = false;

  std::vector<uint64_t> ids;
  ids.reserve(bookmarks.size());
  for (const BookmarkEntry& bookmark : bookmarks) ids.push_back(BookmarkUtil::syncId(bookmark.xpath));

  // Every sync rescans the book's list from the start; lists are short.
  uint64_t cursor = 0;
  do {
    if (!sync.send(url + "?limit=" + std::to_string(PAGE_SIZE) + "&cursor=" + std::to_string(cursor), doc)) {
      return false;
    }
    if (!doc["cursor"].is<uint64_t>() || !doc["more"].is<bool>() || !doc["items"].is<JsonArrayConst>()) return false;
    const uint64_t next = doc["cursor"];
    const JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    if (items.size() > PAGE_SIZE || next < cursor || (items.size() > 0 && next == cursor) ||
        (doc["more"].as<bool>() && items.size() == 0)) {
      LOG_ERR("BKS", "Invalid bookmark pagination");
      return false;
    }
    for (const JsonObjectConst item : items) applyRemote(item, bookmarks, ids, changed);
    cursor = next;
  } while (doc["more"].as<bool>());

  return !changed || BookmarkFile::saveSynced(bookPath, bookmarks, account);
}
