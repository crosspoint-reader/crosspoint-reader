#include "BookStats.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <ctime>

namespace {
constexpr char BOOK_STATS_FILE[] = "/.crosspoint/book_stats.json";
}  // namespace

BookStats BookStats::instance;

void BookStats::updateBook(const char* path, const char* title, uint32_t sessionSeconds, uint8_t progress) {
  if (!path || path[0] == '\0' || sessionSeconds == 0) return;

  auto it = books.find(path);
  if (it == books.end()) {
    if ((int)books.size() >= MAX_BOOKS) evictOldest();

    BookEntry entry = {};
    if (title && title[0] != '\0') {
      strncpy(entry.title, title, sizeof(entry.title) - 1);
      entry.title[sizeof(entry.title) - 1] = '\0';
    }
    entry.totalSeconds = sessionSeconds;
    entry.sessions = 1;
    entry.progress = progress;
    time_t now;
    time(&now);
    entry.lastReadTimestamp = static_cast<uint32_t>(now);
    books[path] = entry;
  } else {
    BookEntry& entry = it->second;
    if (title && title[0] != '\0') {
      strncpy(entry.title, title, sizeof(entry.title) - 1);
      entry.title[sizeof(entry.title) - 1] = '\0';
    }
    entry.totalSeconds += sessionSeconds;
    entry.sessions++;
    entry.progress = progress;
    time_t now;
    time(&now);
    entry.lastReadTimestamp = static_cast<uint32_t>(now);
  }

  saveToFile();
}

const BookStats::BookEntry* BookStats::getBook(const char* path) const {
  if (!path) return nullptr;
  auto it = books.find(path);
  if (it == books.end()) return nullptr;
  return &it->second;
}

void BookStats::evictOldest() {
  if (books.empty()) return;

  auto oldest = books.begin();
  for (auto it = books.begin(); it != books.end(); ++it) {
    if (it->second.lastReadTimestamp < oldest->second.lastReadTimestamp) {
      oldest = it;
    }
  }
  books.erase(oldest);
}

bool BookStats::saveToFile() {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  JsonObject booksObj = doc["books"].to<JsonObject>();

  for (const auto& kv : books) {
    JsonObject entry = booksObj[kv.first].to<JsonObject>();
    entry["t"] = kv.second.title;
    entry["s"] = kv.second.totalSeconds;
    entry["lr"] = kv.second.lastReadTimestamp;
    entry["p"] = kv.second.progress;
    entry["n"] = kv.second.sessions;
  }

  String json;
  serializeJson(doc, json);
  const bool ok = Storage.writeFile(BOOK_STATS_FILE, json);
  if (!ok) {
    LOG_ERR("BST", "Failed to save book_stats.json");
  }
  return ok;
}

bool BookStats::loadFromFile() {
  if (!Storage.exists(BOOK_STATS_FILE)) {
    return true;
  }

  String json = Storage.readFile(BOOK_STATS_FILE);
  if (json.isEmpty()) {
    LOG_ERR("BST", "Failed to read book_stats.json");
    return false;
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("BST", "JSON parse error: %s", err.c_str());
    return false;
  }

  books.clear();
  JsonObject booksObj = doc["books"].as<JsonObject>();
  bool repaired = false;
  for (JsonPair kv : booksObj) {
    BookEntry entry = {};
    const char* t = kv.value()["t"] | "";
    strncpy(entry.title, t, sizeof(entry.title) - 1);
    entry.title[sizeof(entry.title) - 1] = '\0';
    entry.totalSeconds = kv.value()["s"] | (uint32_t)0;
    entry.lastReadTimestamp = kv.value()["lr"] | (uint32_t)0;
    entry.progress = kv.value()["p"] | (uint8_t)0;
    entry.sessions = kv.value()["n"] | (uint16_t)0;
    if (entry.totalSeconds == 0 && entry.sessions > 0) {
      entry.sessions = 0;
      repaired = true;
    }
    books[kv.key().c_str()] = entry;
  }

  while ((int)books.size() > MAX_BOOKS) evictOldest();

  if (repaired) {
    saveToFile();
    LOG_DBG("BST", "Repaired book_stats (cleared sessions with 0s read time)");
  }

  LOG_DBG("BST", "Loaded %u book entries", (unsigned)books.size());
  return true;
}
