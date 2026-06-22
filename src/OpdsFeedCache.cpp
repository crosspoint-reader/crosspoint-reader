#include "OpdsFeedCache.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace {
// Entry type tokens stored in JSON
constexpr char TYPE_BOOK[] = "b";
constexpr char TYPE_NAV[] = "n";
}  // namespace

std::string OpdsFeedCache::cachePath(const std::string& serverUrl) {
  const uint16_t hash = static_cast<uint16_t>(std::hash<std::string>{}(serverUrl) & 0xFFFF);
  char buf[48];
  snprintf(buf, sizeof(buf), "/.crosspoint/opds_%04x.json", hash);
  return buf;
}

bool OpdsFeedCache::load(const std::string& serverUrl) {
  cachedEntries.clear();
  cachedServerUrl.clear();

  const std::string path = cachePath(serverUrl);
  if (!Storage.exists(path.c_str())) {
    LOG_DBG("OFC", "No cache file: %s", path.c_str());
    return false;
  }

  String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    LOG_ERR("OFC", "Cache file empty: %s", path.c_str());
    return false;
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("OFC", "JSON parse error: %s", err.c_str());
    return false;
  }

  if (doc["v"].as<int>() != CACHE_FORMAT_VERSION) {
    LOG_DBG("OFC", "Cache version mismatch, ignoring");
    return false;
  }

  const std::string storedUrl = doc["url"] | std::string("");
  if (storedUrl != serverUrl) {
    LOG_DBG("OFC", "Cache URL mismatch, ignoring");
    return false;
  }

  JsonArray arr = doc["entries"].as<JsonArray>();
  cachedEntries.reserve(std::min(arr.size(), MAX_CACHED_ENTRIES));

  for (JsonObject obj : arr) {
    if (cachedEntries.size() >= MAX_CACHED_ENTRIES) break;
    OpdsCachedEntry ce;
    const std::string tp = obj["tp"] | std::string("");
    ce.entry.type = (tp == TYPE_BOOK) ? OpdsEntryType::BOOK : OpdsEntryType::NAVIGATION;
    ce.entry.title = obj["ti"] | std::string("");
    ce.entry.author = obj["au"] | std::string("");
    ce.entry.href = obj["hr"] | std::string("");
    ce.entry.id = obj["id"] | std::string("");
    ce.localPath = obj["lp"] | std::string("");
    if (ce.entry.title.empty()) continue;  // skip malformed entries
    cachedEntries.push_back(std::move(ce));
  }

  cachedServerUrl = serverUrl;
  LOG_DBG("OFC", "Loaded %zu entries from %s", cachedEntries.size(), path.c_str());
  return true;  // Parsed OK even if feed is empty
}

bool OpdsFeedCache::saveInternal() const {
  if (cachedServerUrl.empty()) return false;

  Storage.mkdir("/.crosspoint");
  const std::string path = cachePath(cachedServerUrl);

  JsonDocument doc;
  doc["v"] = CACHE_FORMAT_VERSION;
  doc["url"] = cachedServerUrl;

  JsonArray arr = doc["entries"].to<JsonArray>();
  for (const auto& ce : cachedEntries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["tp"] = (ce.entry.type == OpdsEntryType::BOOK) ? TYPE_BOOK : TYPE_NAV;
    obj["ti"] = ce.entry.title;
    if (!ce.entry.author.empty()) obj["au"] = ce.entry.author;
    obj["hr"] = ce.entry.href;
    obj["id"] = ce.entry.id;
    if (!ce.localPath.empty()) obj["lp"] = ce.localPath;
  }

  String json;
  serializeJson(doc, json);
  const bool ok = Storage.writeFile(path.c_str(), json);
  if (!ok) {
    LOG_ERR("OFC", "Failed to write cache: %s", path.c_str());
  }
  return ok;
}

bool OpdsFeedCache::save(const std::string& serverUrl, const std::vector<OpdsEntry>& entries) {
  cachedEntries.clear();
  cachedServerUrl = serverUrl;

  const size_t limit = std::min(entries.size(), MAX_CACHED_ENTRIES);
  cachedEntries.reserve(limit);
  for (size_t i = 0; i < limit; i++) {
    OpdsCachedEntry ce;
    ce.entry = entries[i];
    cachedEntries.push_back(std::move(ce));
  }

  return saveInternal();
}

bool OpdsFeedCache::updateLocalPath(const std::string& entryId, const std::string& path) {
  for (auto& ce : cachedEntries) {
    if (ce.entry.id == entryId) {
      ce.localPath = path;
      return saveInternal();
    }
  }
  LOG_DBG("OFC", "updateLocalPath: id not found: %s", entryId.c_str());
  return false;
}

std::string OpdsFeedCache::getLocalPath(const std::string& entryId) const {
  for (const auto& ce : cachedEntries) {
    if (ce.entry.id == entryId) return ce.localPath;
  }
  return {};
}
