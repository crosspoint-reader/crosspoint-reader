#include "BookFinishedCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <functional>

#include "ReadingStats.h"

namespace {

void writeU32(FsFile& f, uint32_t value) {
  uint8_t data[4];
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
  data[2] = (value >> 16) & 0xFF;
  data[3] = (value >> 24) & 0xFF;
  f.write(data, 4);
}

bool readU32(FsFile& f, uint32_t& value) {
  uint8_t data[4];
  if (f.read(data, 4) != 4) {
    return false;
  }
  value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
          (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}

}  // namespace

BookFinishedCache& BookFinishedCache::getInstance() {
  static BookFinishedCache instance;
  return instance;
}

uint32_t BookFinishedCache::makeKey(const std::string& bookPath) {
  return static_cast<uint32_t>(std::hash<std::string>{}(bookPath));
}

void BookFinishedCache::ensureLoaded() {
  if (loaded) {
    return;
  }
  loadFromDisk();
  loaded = true;
}

bool BookFinishedCache::loadFromDisk() {
  if (!Storage.exists(CACHE_PATH)) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead("BFC", CACHE_PATH, f)) {
    return false;
  }

  bool corrupted = false;

  uint32_t magic;
  if (!readU32(f, magic) || magic != CACHE_MAGIC) {
    corrupted = true;
  }

  uint8_t version = 0;
  if (!corrupted && (f.read(&version, 1) != 1 || version != CACHE_VERSION)) {
    corrupted = true;
  }

  uint8_t count = 0;
  if (!corrupted && f.read(&count, 1) != 1) {
    corrupted = true;
  }

  clear();
  if (!corrupted) {
    const uint8_t clampedCount = (count > MAX_ENTRIES) ? static_cast<uint8_t>(MAX_ENTRIES) : count;
    for (uint8_t i = 0; i < clampedCount; i++) {
      uint32_t key;
      uint32_t age;
      uint8_t value;
      if (!readU32(f, key) || f.read(&value, 1) != 1 || !readU32(f, age)) {
        corrupted = true;
        break;
      }
      entries[i].used = true;
      entries[i].key = key;
      entries[i].value = value ? 1 : 0;
      entries[i].age = age;
      if (age > ageCounter) {
        ageCounter = age;
      }
    }
  }

  f.close();

  if (corrupted) {
    LOG_INF("BFC", "Corrupted finished cache detected at %s; resetting", CACHE_PATH);
    clear();
    if (!Storage.remove(CACHE_PATH)) {
      LOG_ERR("BFC", "Failed to remove corrupted finished cache file: %s", CACHE_PATH);
    }
    return false;
  }

  dirty = false;
  return true;
}

bool BookFinishedCache::saveToDisk() {
  Storage.mkdir("/.crosspoint");

  FsFile f;
  if (!Storage.openFileForWrite("BFC", CACHE_PATH, f)) {
    return false;
  }

  uint8_t usedCount = 0;
  for (const auto& entry : entries) {
    if (entry.used) {
      usedCount++;
    }
  }

  writeU32(f, CACHE_MAGIC);
  uint8_t version = CACHE_VERSION;
  f.write(&version, 1);
  f.write(&usedCount, 1);

  for (const auto& entry : entries) {
    if (!entry.used) {
      continue;
    }
    writeU32(f, entry.key);
    uint8_t value = entry.value == 1 ? 1 : 0;
    f.write(&value, 1);
    writeU32(f, entry.age);
  }

  f.close();
  dirty = false;
  return true;
}

int BookFinishedCache::findIndex(const uint32_t key) const {
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].used && entries[i].key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int BookFinishedCache::findEvictionIndex() const {
  int freeIndex = -1;
  uint32_t oldestAge = UINT32_MAX;
  int oldestIndex = 0;

  for (size_t i = 0; i < entries.size(); i++) {
    if (!entries[i].used) {
      freeIndex = static_cast<int>(i);
      break;
    }
    if (entries[i].age < oldestAge) {
      oldestAge = entries[i].age;
      oldestIndex = static_cast<int>(i);
    }
  }

  return (freeIndex >= 0) ? freeIndex : oldestIndex;
}

bool BookFinishedCache::loadFinishedFromStats(const std::string& bookPath, bool& finished) {
  const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
  BookStats stats;
  if (stats.loadFromFile(cachePath + "/stats.json")) {
    finished = stats.finished;
    return true;
  }

  finished = false;
  return false;
}

bool BookFinishedCache::tryGet(const std::string& bookPath, bool& finished) {
  ensureLoaded();

  const uint32_t key = makeKey(bookPath);
  const int idx = findIndex(key);
  if (idx < 0) {
    return false;
  }

  entries[idx].age = ++ageCounter;
  finished = (entries[idx].value == 1);
  return true;
}

bool BookFinishedCache::resolve(const std::string& bookPath, bool& finished) {
  ensureLoaded();

  if (tryGet(bookPath, finished)) {
    return true;
  }

  const bool loaded = loadFinishedFromStats(bookPath, finished);
  put(bookPath, finished);
  return loaded;
}

void BookFinishedCache::put(const std::string& bookPath, const bool finished) {
  ensureLoaded();

  const uint32_t key = makeKey(bookPath);
  int idx = findIndex(key);
  if (idx < 0) {
    idx = findEvictionIndex();
    entries[idx].used = true;
    entries[idx].key = key;
  }

  entries[idx].value = finished ? 1 : 0;
  entries[idx].age = ++ageCounter;
  dirty = true;
}

void BookFinishedCache::clear() {
  for (auto& entry : entries) {
    entry = Entry{};
  }
  ageCounter = 0;
  dirty = false;
}

bool BookFinishedCache::saveIfDirty() {
  ensureLoaded();
  if (!dirty) {
    return true;
  }
  return saveToDisk();
}
