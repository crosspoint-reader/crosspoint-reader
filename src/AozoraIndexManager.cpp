#include "AozoraIndexManager.h"
#include <ArduinoJson.h>
#include <Logging.h>

bool AozoraIndexManager::loadAndPurge() {
  entries_.clear();

  FsFile file;
  if (!Storage.openFileForRead("AOZORA", INDEX_PATH, file)) {
    LOG_DBG("AOZORA", "No index file, starting fresh");
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_ERR("AOZORA", "Index parse error: %s", err.c_str());
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());

  bool needsSave = false;
  for (JsonObject obj : arr) {
    AozoraBookEntry entry;
    entry.workId = obj["work_id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (const char*)(obj["author"] | ""));
    snprintf(entry.filename, sizeof(entry.filename), "%s", (const char*)(obj["filename"] | ""));

    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);

    if (Storage.exists(fullPath)) {
      entries_.push_back(entry);
    } else {
      LOG_DBG("AOZORA", "Purging missing: %s", entry.filename);
      needsSave = true;
    }
  }

  if (needsSave) {
    saveIndex();
  }

  return true;
}

bool AozoraIndexManager::isDownloaded(int workId) const {
  for (const auto& e : entries_) {
    if (e.workId == workId) return true;
  }
  return false;
}

bool AozoraIndexManager::addEntry(int workId, const char* title, const char* author, const char* filename) {
  if (isDownloaded(workId)) return true;

  AozoraBookEntry entry;
  entry.workId = workId;
  snprintf(entry.title, sizeof(entry.title), "%s", title);
  snprintf(entry.author, sizeof(entry.author), "%s", author);
  snprintf(entry.filename, sizeof(entry.filename), "%s", filename);
  entries_.push_back(entry);

  return saveIndex();
}

bool AozoraIndexManager::removeEntry(int workId) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->workId == workId) {
      char fullPath[160];
      snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, it->filename);
      Storage.remove(fullPath);
      entries_.erase(it);
      return saveIndex();
    }
  }
  return false;
}

bool AozoraIndexManager::saveIndex() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : entries_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["work_id"] = e.workId;
    obj["title"] = e.title;
    obj["author"] = e.author;
    obj["filename"] = e.filename;
  }

  FsFile file;
  if (!Storage.openFileForWrite("AOZORA", INDEX_PATH, file)) {
    LOG_ERR("AOZORA", "Failed to open index for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}

static void sanitizeForFat32(const char* src, char* dest, size_t destSize) {
  size_t pos = 0;
  for (size_t i = 0; src[i] && pos < destSize - 1; i++) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    if (c == '<' || c == '>' || c == ':' || c == '"' ||
        c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
      dest[pos++] = '_';
    } else {
      dest[pos++] = static_cast<char>(c);
    }
  }
  dest[pos] = '\0';
}

std::string AozoraIndexManager::makeRelativePath(int workId, const char* title, const char* author) {
  char safeAuthor[32];
  sanitizeForFat32(author, safeAuthor, sizeof(safeAuthor));

  char safeTitle[52];
  sanitizeForFat32(title, safeTitle, sizeof(safeTitle));

  char result[160];
  snprintf(result, sizeof(result), "%s/%d_%s.epub", safeAuthor, workId, safeTitle);
  return std::string(result);
}

bool AozoraIndexManager::ensureAuthorDirectory(const char* author) {
  char safeAuthor[32];
  sanitizeForFat32(author, safeAuthor, sizeof(safeAuthor));

  char dirPath[80];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", AOZORA_DIR, safeAuthor);

  if (Storage.exists(dirPath)) return true;
  return Storage.mkdir(dirPath);
}

bool AozoraIndexManager::ensureDirectory() {
  if (Storage.exists(AOZORA_DIR)) return true;
  return Storage.mkdir(AOZORA_DIR);
}
