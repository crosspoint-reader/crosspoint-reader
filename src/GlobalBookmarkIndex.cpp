#include "GlobalBookmarkIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

GlobalBookmarkIndex GlobalBookmarkIndex::instance;

namespace {
void writeString(FsFile& f, const std::string& s) {
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&len), sizeof(len));
  if (len > 0) {
    f.write(reinterpret_cast<const uint8_t*>(s.data()), len);
  }
}

bool readString(FsFile& f, std::string& out) {
  uint16_t len = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len)) return false;
  out.clear();
  if (len == 0) return true;
  out.resize(len);
  return f.read(reinterpret_cast<uint8_t*>(&out[0]), len) == len;
}
}  // namespace

std::vector<GlobalBookmarkIndex::Entry>::iterator GlobalBookmarkIndex::findBySourcePath(const std::string& sourcePath) {
  return std::find_if(entries.begin(), entries.end(),
                      [&sourcePath](const Entry& e) { return e.sourcePath == sourcePath; });
}

void GlobalBookmarkIndex::load() {
  entries.clear();
  loaded = true;

  FsFile f;
  if (!Storage.openFileForRead("GBI", FILE_PATH, f)) {
    LOG_DBG("GBI", "No existing global bookmarks file");
    return;
  }

  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != FILE_VERSION) {
    LOG_ERR("GBI", "Bad version: %u", version);
    f.close();
    return;
  }

  uint16_t entryCount = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&entryCount), sizeof(entryCount)) != sizeof(entryCount)) {
    f.close();
    return;
  }

  entries.reserve(entryCount);
  for (uint16_t i = 0; i < entryCount; i++) {
    Entry e;
    uint8_t isTxtByte = 0;
    if (!readString(f, e.sourcePath) || !readString(f, e.cacheDir) || !readString(f, e.title) ||
        f.read(&isTxtByte, 1) != 1) {
      LOG_ERR("GBI", "Truncated entry %u", i);
      entries.clear();
      f.close();
      return;
    }
    e.isTxt = (isTxtByte != 0);

    uint16_t bmCount = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&bmCount), sizeof(bmCount)) != sizeof(bmCount)) {
      entries.clear();
      f.close();
      return;
    }
    e.bookmarks.reserve(bmCount);
    for (uint16_t j = 0; j < bmCount; j++) {
      BookmarkStore::Bookmark bm;
      if (f.read(reinterpret_cast<uint8_t*>(&bm.spineIndex), sizeof(bm.spineIndex)) != sizeof(bm.spineIndex) ||
          f.read(reinterpret_cast<uint8_t*>(&bm.pageNumber), sizeof(bm.pageNumber)) != sizeof(bm.pageNumber) ||
          !readString(f, bm.name)) {
        LOG_ERR("GBI", "Truncated bookmark");
        entries.clear();
        f.close();
        return;
      }
      e.bookmarks.push_back(std::move(bm));
    }
    entries.push_back(std::move(e));
  }
  f.close();
  LOG_DBG("GBI", "Loaded %u entries", static_cast<unsigned>(entries.size()));
}

void GlobalBookmarkIndex::save() const {
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (!Storage.openFileForWrite("GBI", FILE_PATH, f)) {
    LOG_ERR("GBI", "Failed to open for write");
    return;
  }

  const uint8_t version = FILE_VERSION;
  f.write(&version, 1);
  const uint16_t entryCount = static_cast<uint16_t>(std::min<size_t>(entries.size(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&entryCount), sizeof(entryCount));

  for (uint16_t i = 0; i < entryCount; i++) {
    const Entry& e = entries[i];
    writeString(f, e.sourcePath);
    writeString(f, e.cacheDir);
    writeString(f, e.title);
    const uint8_t isTxtByte = e.isTxt ? 1 : 0;
    f.write(&isTxtByte, 1);
    const uint16_t bmCount = static_cast<uint16_t>(std::min<size_t>(e.bookmarks.size(), UINT16_MAX));
    f.write(reinterpret_cast<const uint8_t*>(&bmCount), sizeof(bmCount));
    for (const auto& bm : e.bookmarks) {
      f.write(reinterpret_cast<const uint8_t*>(&bm.spineIndex), sizeof(bm.spineIndex));
      f.write(reinterpret_cast<const uint8_t*>(&bm.pageNumber), sizeof(bm.pageNumber));
      writeString(f, bm.name);
    }
  }
  f.close();
  LOG_DBG("GBI", "Saved %u entries", static_cast<unsigned>(entryCount));
}

void GlobalBookmarkIndex::upsertFromStore(const std::string& sourcePath, const std::string& cacheDir,
                                          const std::string& title, bool isTxt,
                                          const std::vector<BookmarkStore::Bookmark>& bookmarks) {
  if (!loaded) load();

  auto it = findBySourcePath(sourcePath);
  if (bookmarks.empty()) {
    if (it != entries.end()) {
      entries.erase(it);
      save();
    }
    return;
  }

  if (it == entries.end()) {
    Entry e;
    e.sourcePath = sourcePath;
    e.cacheDir = cacheDir;
    e.title = title;
    e.isTxt = isTxt;
    e.bookmarks = bookmarks;
    entries.push_back(std::move(e));
  } else {
    it->cacheDir = cacheDir;
    it->title = title;
    it->isTxt = isTxt;
    it->bookmarks = bookmarks;
  }
  save();
}

void GlobalBookmarkIndex::syncFromStore(const BookmarkStore& store, const std::string& sourcePath,
                                        const std::string& cacheDir, const std::string& title, bool isTxt) {
  upsertFromStore(sourcePath, cacheDir, title, isTxt, store.getAll());
}

void GlobalBookmarkIndex::removeBySourcePath(const std::string& sourcePath) {
  if (!loaded) load();
  auto it = findBySourcePath(sourcePath);
  if (it != entries.end()) {
    entries.erase(it);
    save();
  }
}

bool GlobalBookmarkIndex::reconcile() {
  if (!loaded) load();
  const size_t before = entries.size();
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const Entry& e) {
                                 if (!Storage.exists(e.sourcePath.c_str())) {
                                   LOG_DBG("GBI", "Dropping orphan entry: %s", e.sourcePath.c_str());
                                   return true;
                                 }
                                 return false;
                               }),
                entries.end());
  const bool changed = entries.size() != before;
  if (changed) save();
  return changed;
}

size_t GlobalBookmarkIndex::totalBookmarkCount() const {
  size_t total = 0;
  for (const auto& e : entries) total += e.bookmarks.size();
  return total;
}
