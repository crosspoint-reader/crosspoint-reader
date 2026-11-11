#include "FavoritesStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace {
// `{"books":[]}`: the document around the entries.
constexpr size_t DOC_FRAME_BYTES = 12;
// Headroom kept free beyond a load step's own worst case.
constexpr size_t HEAP_MARGIN_BYTES = 1024;

// Length of the first maxBytes of s (or all of it), not splitting a UTF-8
// sequence. Reads at most maxBytes + 1 bytes of s.
size_t clampedLength(const char* s, const size_t len, const size_t maxBytes) {
  if (len <= maxBytes) return len;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) end--;
  return end;
}

std::string clampUtf8(const std::string& s, const size_t maxBytes) {
  return s.substr(0, clampedLength(s.data(), s.size(), maxBytes));
}

// Builds the clamped string straight from a C string, so an oversized value
// (from a hand-edited file) is never copied in full first.
std::string clampUtf8(const char* s, const size_t maxBytes) {
  const size_t len = strnlen(s, maxBytes + 1);
  return std::string(s, clampedLength(s, len, maxBytes));
}

// The first maxBytes of s (or all of it) as a view, not splitting a UTF-8
// sequence: what clampUtf8 would keep, without copying it.
std::string_view clampedView(const std::string& s, const size_t maxBytes) {
  return std::string_view(s.data(), clampedLength(s.data(), s.size(), maxBytes));
}

// Serialized size of one JSON string value as ArduinoJson 7 writes it
// (TextFormatter::writeChar): quotes, \" \\ \b \f \n \r \t as two bytes, NUL as
// \u0000, everything else as is. Checked against measureJson() on 20,000
// random entries with every escape class.
size_t jsonStringBytes(const std::string_view s) {
  size_t n = 2;
  for (const char c : s) {
    switch (c) {
      case '"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
        n += 2;
        break;
      case '\0':
        n += 6;
        break;
      default:
        n += 1;
    }
  }
  return n;
}
}  // namespace

size_t FavoritesStore::entryBytes(const std::string_view path, const std::string_view title,
                                  const std::string_view author, const bool resolved) {
  // Exactly what toJson() writes for one entry, computed rather than built:
  // `{"path":` `,"title":` `,"author":` `,"resolved":` `}` is 40 bytes, then
  // the three strings and true/false. No allocation, so it cannot come out
  // short under memory pressure.
  return 40 + jsonStringBytes(path) + jsonStringBytes(title) + jsonStringBytes(author) + (resolved ? 4 : 5);
}

void FavoritesStore::setFields(FavoriteBook& book, const std::string& title, const std::string& author,
                               const bool resolved) {
  book.title = clampUtf8(title, MAX_TITLE_BYTES);
  book.author = clampUtf8(author, MAX_AUTHOR_BYTES);
  book.resolved = resolved;
  book.jsonBytes = entryBytes(book.path, book.title, book.author, book.resolved);
}

std::unique_ptr<FavoriteBook> FavoritesStore::makeBook(const std::string& path, const std::string& title,
                                                       const std::string& author, const bool resolved) {
  if (path.empty() || path.size() > MAX_PATH_BYTES) return nullptr;
  auto book = makeUniqueNoThrow<FavoriteBook>();
  if (!book) return book;
  book->path = path;
  setFields(*book, title, author, resolved);
  return book;
}

bool FavoritesStore::reserveList() {
  if (favorites.capacity() >= static_cast<size_t>(MAX_FAVORITES)) return true;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MAX_FAVORITES * sizeof(favorites[0]) + HEAP_MARGIN_BYTES) {
    return false;
  }
  favorites.reserve(MAX_FAVORITES);
  return true;
}

bool FavoritesStore::roomForBook(const size_t pathLen) {
  // Worst case live at once while a book is built: the path twice (the
  // caller's temporary and the kept copy), the clamped title and author twice
  // each, and the book itself.
  const size_t need = 2 * (pathLen + MAX_TITLE_BYTES + MAX_AUTHOR_BYTES) + sizeof(FavoriteBook);
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= need + HEAP_MARGIN_BYTES;
}

size_t FavoritesStore::memBytes() const {
  size_t total = DOC_FRAME_BYTES;
  for (const auto& book : favorites) total += book->jsonBytes;
  if (!favorites.empty()) total += favorites.size() - 1;  // commas
  return total;
}

FavoritesStore::ReadResult FavoritesStore::readOnce() {
  if (!Storage.exists(getFilePath())) return ReadResult::Missing;
  size_t size = 0;
  {
    HalFile file;
    if (!Storage.openFileForRead("FAV", getFilePath(), file)) return ReadResult::Transient;
    size = file.fileSize();
  }
  // readFile() returns at most this many bytes; a bigger file can never load,
  // which makes it unusable rather than unlucky. This store never writes one.
  if (size > READ_LIMIT_BYTES) return ReadResult::Corrupt;
  JsonDocument doc;
  DeserializationError error;
  {
    const String json = Storage.readFile(getFilePath());
    // Short read: readFile() grows its String a byte at a time and stops
    // silently when that fails, or the card hiccuped. Not the file's fault.
    if (json.length() != size) return ReadResult::Transient;
    // The document copies the strings out of a read-only input, so the file
    // text is freed here, before fromJson() builds the list.
    error = deserializeJson(doc, json);
  }
  if (error == DeserializationError::NoMemory) return ReadResult::Transient;
  if (error) {
    LOG_ERR("FAV", "favorites.json parse error: %s", error.c_str());
    return ReadResult::Corrupt;
  }
  // Valid JSON that is not a favorites list (no "books" array) is set aside
  // like a parse error, rather than read as an empty list and overwritten.
  if (!doc["books"].is<JsonArrayConst>()) {
    LOG_ERR("FAV", "favorites.json has no books array");
    return ReadResult::Corrupt;
  }
  std::lock_guard<std::mutex> lock(storeMutex);
  loadOom = false;
  fromJson(doc.as<JsonVariantConst>());
  if (!loadOom) return ReadResult::Ok;
  // Free the partial list now, so it does not crowd the retry's read.
  favorites.clear();
  return ReadResult::Transient;
}

void FavoritesStore::load() {
  locked = false;
  absentAtLoad = false;
  // Three tries: one bad read is not a bad file.
  ReadResult result = ReadResult::Transient;
  for (int attempt = 0; attempt < 3; attempt++) {
    result = readOnce();
    if (result == ReadResult::Ok) return;
    if (result == ReadResult::Missing) {
      // No file (first use), or a card that cannot be read right now. Either
      // way memory is empty; refreshFromCard() reloads before the first read
      // or change if the file turns out to be there after all.
      favorites.clear();
      absentAtLoad = true;
      return;
    }
    if (result == ReadResult::Corrupt) break;
  }
  favorites.clear();
  if (result == ReadResult::Transient) {
    // Out of memory or an unreliable read: the file is probably fine and the
    // next boot will load it. Never save over it from a partial list.
    locked = true;
    LOG_ERR("FAV", "favorites.json could not be read completely; favorites are read-only this boot");
    return;
  }
  // Read in full and still not a favorites list: set it aside (kept on the
  // card for recovery from a PC) and start empty, instead of leaving favorites
  // dead. An earlier set-aside file is never overwritten; with one already
  // there, lock instead.
  const std::string bad = std::string(getFilePath()) + ".bad";
  if (!Storage.exists(bad.c_str()) && Storage.rename(getFilePath(), bad.c_str())) {
    LOG_ERR("FAV", "favorites.json unreadable; moved to %s, starting empty", bad.c_str());
  } else {
    locked = true;
    LOG_ERR("FAV", "favorites.json unreadable and not set aside; favorites are read-only this boot");
  }
}

bool FavoritesStore::canAdd(const std::string& path, const std::string& title, const std::string& author) const {
  if (locked || getCount() >= MAX_FAVORITES || path.empty() || path.size() > MAX_PATH_BYTES) return false;
  const size_t entry =
      entryBytes(path, clampedView(title, MAX_TITLE_BYTES), clampedView(author, MAX_AUTHOR_BYTES), /*resolved=*/false);
  // +1 for the comma that joins it to the list.
  return memBytes() + entry + 1 <= ADD_FILE_BYTES;
}

void FavoritesStore::refreshFromCard() {
  if (!absentAtLoad || !Storage.exists(getFilePath())) return;
  // The file was missing when we loaded but is here now: the card was not
  // readable at boot. Load it before changing anything, or the first save
  // would replace it with the empty list in RAM.
  LOG_INF("FAV", "favorites.json appeared after boot; reloading");
  load();
}

FavoritesStore::SaveResult FavoritesStore::persist() {
  SaveResult result = SaveResult::Failed;
  if (locked) {
    LOG_ERR("FAV", "Favorites are read-only this boot; not saving");
  } else if (memBytes() > MAX_FILE_BYTES) {
    // Stay under the card reader's 50,000-byte readFile() limit, or the next
    // boot could not load what this save wrote.
    LOG_ERR("FAV", "Favorites would be %u bytes (limit %u); not saving", static_cast<unsigned>(memBytes()),
            static_cast<unsigned>(MAX_FILE_BYTES));
    result = SaveResult::TooBig;
  } else {
    std::lock_guard<std::mutex> lock(storeMutex);
    JsonDocument doc;
    toJson(doc);
    if (writeDocToFile(getFilePath(), doc)) {
      result = SaveResult::Saved;
      absentAtLoad = false;
    }
  }
  unsaved = result != SaveResult::Saved;
  if (unsaved) LOG_ERR("FAV", "Favorites not saved; will retry");
  return result;
}

void FavoritesStore::flushPending() {
  if (unsaved) persist();
}

int FavoritesStore::find(const std::string& path) const {
  for (size_t i = 0; i < favorites.size(); i++) {
    if (favorites[i]->path == path) return static_cast<int>(i);
  }
  return -1;
}

void FavoritesStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : favorites) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book->path;
    obj["title"] = book->title;
    obj["author"] = book->author;
    obj["resolved"] = book->resolved;
  }
}

bool FavoritesStore::fromJson(JsonVariantConst doc) {
  // A missing or malformed 'books' key reads as an empty list; only a parse
  // error is fatal (reported before this runs).
  favorites.clear();
  if (!reserveList()) {
    LOG_ERR("FAV", "OOM reserving the favorites list");
    loadOom = true;
    return true;
  }
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_FAVORITES) break;
    const char* path = obj["path"] | "";
    const size_t pathLen = strlen(path);
    // Entries this store would never write (no path, an oversized path, a
    // duplicate) are skipped rather than allocated.
    if (pathLen == 0 || pathLen > MAX_PATH_BYTES) continue;
    // Compared as a view: a duplicate costs no allocation, so it is skipped
    // before the heap check rather than failing it.
    const std::string_view pathView(path, pathLen);
    if (std::any_of(favorites.begin(), favorites.end(),
                    [&](const std::unique_ptr<FavoriteBook>& book) { return book->path == pathView; })) {
      continue;
    }
    if (!roomForBook(pathLen)) {
      LOG_ERR("FAV", "OOM loading favorites after %d entries", getCount());
      loadOom = true;
      break;
    }
    auto book = makeBook(path, clampUtf8(obj["title"] | "", MAX_TITLE_BYTES),
                         clampUtf8(obj["author"] | "", MAX_AUTHOR_BYTES), obj["resolved"] | false);
    if (!book) {
      LOG_ERR("FAV", "OOM loading favorites after %d entries", getCount());
      loadOom = true;
      break;
    }
    favorites.push_back(std::move(book));
  }
  LOG_DBG("FAV", "Favorites loaded from file (%d entries, %u bytes)", getCount(), static_cast<unsigned>(memBytes()));
  return true;
}

bool FavoritesStore::toggle(const std::string& path, const std::string& title, const std::string& author,
                            const bool resolved) {
  refreshFromCard();
  // A save that fails on the card rolls the toggle back. The store stays
  // marked unsaved afterwards: a failed save can leave no file at all
  // (SDCardManager::writeFile removes the old file, then its temp copy, when
  // the final rename fails), and writing the rolled-back list again is always
  // correct, so flushPending() retries it.
  const int found = find(path);
  if (found >= 0) {
    const auto pos = favorites.begin() + found;
    std::unique_ptr<FavoriteBook> removed = std::move(*pos);
    favorites.erase(pos);
    const SaveResult saved = persist();
    // A list that is over the size limit is saved again once it fits, and a
    // removal is how it gets there: keep it rather than undo it.
    if (saved != SaveResult::Failed) return false;
    LOG_ERR("FAV", "Removal of %s not saved; keeping it", path.c_str());
    favorites.insert(favorites.begin() + found, std::move(removed));
    return true;
  }
  if (!canAdd(path, title, author)) {
    LOG_ERR("FAV", "Favorites full (%d entries, %u bytes) or read-only; not adding %s", getCount(),
            static_cast<unsigned>(memBytes()), path.c_str());
    return false;
  }
  if (!reserveList() || !roomForBook(path.size())) {
    LOG_ERR("FAV", "OOM adding favorite %s", path.c_str());
    return false;
  }
  auto book = makeBook(path, title, author, resolved);
  if (!book) {
    LOG_ERR("FAV", "OOM adding favorite %s", path.c_str());
    return false;
  }
  favorites.push_back(std::move(book));
  if (persist() == SaveResult::Saved) return true;
  LOG_ERR("FAV", "New favorite %s not saved; not adding it", path.c_str());
  favorites.pop_back();
  return false;
}

bool FavoritesStore::removeByPath(const std::string& path) {
  refreshFromCard();
  if (path.empty()) return false;
  const bool isDir = path.back() == '/';
  // The book itself, or every favorite under it when it is a folder.
  const size_t before = favorites.size();
  favorites.erase(std::remove_if(favorites.begin(), favorites.end(),
                                 [&](const std::unique_ptr<FavoriteBook>& book) {
                                   const std::string& p = book->path;
                                   if (p == path) return true;
                                   if (p.size() <= path.size() || p.compare(0, path.size(), path) != 0) return false;
                                   return isDir || p[path.size()] == '/';
                                 }),
                  favorites.end());
  if (favorites.size() == before) return false;
  // The book is gone, so memory is right; a failed save is retried later.
  persist();
  return true;
}

void FavoritesStore::forgetIfGone(const char* pathC) {
  if (pathC == nullptr || pathC[0] == '\0') return;
  std::string_view view(pathC);
  while (view.size() > 1 && view.back() == '/') view.remove_suffix(1);
  // No stored path is longer than the limit, so nothing can be this book or
  // lie under this folder.
  if (view.size() > MAX_PATH_BYTES) return;
  const std::string path(view);
  // Delete paths can report failure after the item itself is gone (a sidecar
  // cleanup failed), so ask the card. Its parent folder still answering shows
  // the card responded, so a read glitch is not taken for a delete.
  if (Storage.exists(path.c_str())) {
    // Still there: a book whose delete failed outright (keep it), or a folder
    // whose recursive delete stopped partway. For a folder, drop the
    // favorites inside it whose books are already gone; the folder itself
    // answering shows the card responded.
    refreshFromCard();
    // Two passes, as in pruneMissing(): find the missing books, then confirm
    // the card still answers before dropping any, so a card that drops out
    // during the scan cannot turn the rest of the folder into "missing".
    bool missing[MAX_FAVORITES] = {};
    const size_t count = std::min(favorites.size(), static_cast<size_t>(MAX_FAVORITES));
    size_t missingCount = 0;
    for (size_t i = 0; i < count; i++) {
      const std::string& p = favorites[i]->path;
      missing[i] = p.size() > path.size() && p.compare(0, path.size(), path) == 0 && p[path.size()] == '/' &&
                   !Storage.exists(p.c_str());
      if (missing[i]) missingCount++;
    }
    if (missingCount == 0 || !Storage.exists(path.c_str()) || !Storage.exists(getFilePath())) return;
    size_t keep = 0;
    for (size_t i = 0; i < favorites.size(); i++) {
      if (i < count && missing[i]) continue;
      if (keep != i) favorites[keep] = std::move(favorites[i]);
      keep++;
    }
    favorites.resize(keep);
    persist();
    return;
  }
  const size_t slash = path.rfind('/');
  const std::string parent = (slash == std::string::npos || slash == 0) ? std::string("/") : path.substr(0, slash);
  if (!Storage.exists(parent.c_str())) return;
  removeByPath(path);
}

void FavoritesStore::updatePath(const char* oldPathC, const char* newPathC) {
  if (oldPathC == nullptr || newPathC == nullptr) return;
  // Views over the caller's buffers: nothing here copies a path until its
  // final size is known to be within MAX_PATH_BYTES.
  const std::string_view oldPath(oldPathC);
  const std::string_view newPath(newPathC);
  if (oldPath.empty() || newPath.empty() || oldPath == newPath) return;
  // No stored path is longer than the limit, so a longer oldPath matches
  // nothing: neither the book itself nor (as a folder) anything under it.
  if (oldPath.size() > MAX_PATH_BYTES) return;
  refreshFromCard();
  const bool fitBefore = memBytes() <= MAX_FILE_BYTES;
  const bool oldHasSlash = oldPath.back() == '/';
  const bool newHasSlash = newPath.back() == '/';
  const size_t oldDirLen = oldPath.size() + (oldHasSlash ? 0 : 1);
  const size_t newDirLen = newPath.size() + (newHasSlash ? 0 : 1);
  // p lies under folder `dir` (given without a trailing slash requirement).
  const auto under = [](const std::string& p, const std::string_view dir, const bool dirHasSlash) {
    if (p.size() <= dir.size() || p.compare(0, dir.size(), dir) != 0) return false;
    return dirHasSlash || p[dir.size()] == '/';
  };
  const auto moving = [&](const std::string& p) { return p == oldPath || under(p, oldPath, oldHasSlash); };
  // A move that overwrote its destination (WebDAV Overwrite: T) leaves the
  // destination's old favorite pointing at the new content; drop it so the
  // moved entry is the only one at that path.
  const size_t before = favorites.size();
  favorites.erase(std::remove_if(favorites.begin(), favorites.end(),
                                 [&](const std::unique_ptr<FavoriteBook>& book) {
                                   return !moving(book->path) &&
                                          (book->path == newPath || under(book->path, newPath, newHasSlash));
                                 }),
                  favorites.end());
  bool changed = favorites.size() != before;
  for (auto it = favorites.begin(); it != favorites.end();) {
    FavoriteBook& book = **it;
    const bool exact = book.path == oldPath;
    if (!exact && !under(book.path, oldPath, oldHasSlash)) {
      ++it;
      continue;
    }
    changed = true;
    // Size the new path before building it, so an oversized one is never
    // allocated.
    const size_t newSize = exact ? newPath.size() : newDirLen + (book.path.size() - oldDirLen);
    if (newSize > MAX_PATH_BYTES) {
      // The load would skip a path this long, so the favorite would vanish
      // silently at the next boot; drop it now and say so.
      LOG_ERR("FAV", "Moved favorite's path would be %u bytes (limit %u); removing it", static_cast<unsigned>(newSize),
              static_cast<unsigned>(MAX_PATH_BYTES));
      it = favorites.erase(it);
      continue;
    }
    if (exact) {
      book.path.assign(newPath.data(), newPath.size());
    } else {
      std::string moved;
      moved.reserve(newSize);
      moved.append(newPath.data(), newPath.size());
      if (!newHasSlash) moved.push_back('/');
      moved.append(book.path, oldDirLen, std::string::npos);
      book.path = std::move(moved);
    }
    // The title may have come from the old file name; let the Library re-read
    // it, and look the new path up even if the old one was not found.
    book.lookupTried = false;
    setFields(book, book.title, book.author, /*resolved=*/false);
    ++it;
  }
  // Longer paths can push a full list past what a save may write; such a list
  // could never be saved, so the next boot would load the old paths and prune
  // every moved favorite. When this move caused that (the list fit before),
  // drop moved favorites, newest first, until it fits: their growth is the
  // whole excess, so dropping them always gets there. A list that was already
  // too big is left alone; removing other favorites lets it save later.
  size_t dropped = 0;
  for (size_t i = favorites.size(); fitBefore && i > 0 && memBytes() > MAX_FILE_BYTES; i--) {
    const std::string& p = favorites[i - 1]->path;
    if (p != newPath && !under(p, newPath, newHasSlash)) continue;
    favorites.erase(favorites.begin() + static_cast<std::ptrdiff_t>(i - 1));
    dropped++;
  }
  if (dropped > 0) {
    LOG_ERR("FAV", "Moved favorites did not fit in %u bytes; removed %u of them", static_cast<unsigned>(MAX_FILE_BYTES),
            static_cast<unsigned>(dropped));
  }
  // The files already moved, so memory is right; a failed save is retried by
  // flushPending() (the Library calls it on entry) rather than rolled back.
  if (changed) persist();
}

bool FavoritesStore::setResolved(const size_t i, const std::string& title, const std::string& author) {
  if (i >= favorites.size()) return false;
  FavoriteBook& book = *favorites[i];
  const size_t now = book.jsonBytes;
  const size_t next =
      entryBytes(book.path, clampedView(title, MAX_TITLE_BYTES), clampedView(author, MAX_AUTHOR_BYTES), true);
  // Adopting a longer title must not push the list past the add limit: the
  // space above it is reserved for path repoints, which cannot be refused.
  if (next > now && memBytes() - now + next > ADD_FILE_BYTES) return false;
  setFields(book, title, author, /*resolved=*/true);
  unsaved = true;
  return true;
}

void FavoritesStore::markLookupTried(const size_t i) {
  if (i < favorites.size()) favorites[i]->lookupTried = true;
}

void FavoritesStore::invalidateResolved() {
  for (auto& book : favorites) {
    book->lookupTried = false;
    setFields(*book, book->title, book->author, /*resolved=*/false);
  }
  // Persist the cleared flags too, so an entry whose lookup fails now is
  // looked up again after a reboot instead of trusting its old copy.
  if (!favorites.empty()) unsaved = true;
}

bool FavoritesStore::pruneMissing() {
  // Every exists() fails while the card is unreadable; that is not every
  // favorite being deleted. Prune only when the card's own state folder is
  // visible, so a glitch cannot empty the list.
  if (favorites.empty() || !Storage.exists("/.crosspoint")) return false;
  // Two passes: find the missing books, then confirm the card is still
  // readable before dropping any, so a card that drops out mid-pass cannot
  // turn every later check into a "missing". (A card that drops out and comes
  // back between the two confirming checks is not detected.)
  bool missing[MAX_FAVORITES] = {};
  const size_t count = std::min(favorites.size(), static_cast<size_t>(MAX_FAVORITES));
  size_t missingCount = 0;
  for (size_t i = 0; i < count; i++) {
    missing[i] = !Storage.exists(favorites[i]->path.c_str());
    if (missing[i]) missingCount++;
  }
  if (missingCount == 0 || !Storage.exists("/.crosspoint") || !Storage.exists(getFilePath())) return false;
  size_t keep = 0;
  for (size_t i = 0; i < favorites.size(); i++) {
    if (i < count && missing[i]) continue;
    if (keep != i) favorites[keep] = std::move(favorites[i]);
    keep++;
  }
  favorites.resize(keep);
  unsaved = true;
  return true;
}
