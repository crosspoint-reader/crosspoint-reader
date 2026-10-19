#include "OpdsTokenStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>

namespace {
// Access/refresh tokens (JWTs) run long; cap the decode to avoid a malformed
// file ballooning the heap.
constexpr size_t MAX_TOKEN_CHARS = 2048;
}  // namespace

void OpdsTokenStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["tokens"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["url"] = entry.url;
    obj["access_obf"] = obfuscation::obfuscateToBase64(entry.tokens.accessToken);
    obj["refresh_obf"] = obfuscation::obfuscateToBase64(entry.tokens.refreshToken);
    obj["refresh_url"] = entry.tokens.refreshUrl;
  }
}

bool OpdsTokenStore::fromJson(JsonVariantConst doc) {
  entries.clear();
  JsonArrayConst arr = doc["tokens"].as<JsonArrayConst>();
  entries.reserve(std::min(arr.size(), MAX_SERVERS));
  for (JsonObjectConst obj : arr) {
    if (entries.size() >= MAX_SERVERS) break;
    Entry entry;
    entry.url = obj["url"] | "";
    if (entry.url.empty()) continue;
    bool ok = false;
    bool tooLong = false;
    entry.tokens.accessToken =
        obfuscation::deobfuscateFromBase64(obj["access_obf"] | "", MAX_TOKEN_CHARS, &ok, &tooLong);
    entry.tokens.refreshToken =
        obfuscation::deobfuscateFromBase64(obj["refresh_obf"] | "", MAX_TOKEN_CHARS, &ok, &tooLong);
    entry.tokens.refreshUrl = obj["refresh_url"] | "";
    entries.push_back(std::move(entry));
  }
  LOG_DBG("OPDSTOK", "Loaded %zu OPDS token entries", entries.size());
  return true;
}

OpdsTokens OpdsTokenStore::get(const std::string& url) const {
  for (const auto& entry : entries) {
    if (entry.url == url) return entry.tokens;
  }
  return {};
}

bool OpdsTokenStore::put(const std::string& url, const OpdsTokens& tokens) {
  const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.url == url; });

  if (tokens.empty()) {
    // Clearing: drop the entry if present; nothing to write otherwise.
    if (it == entries.end()) return true;
    entries.erase(it);
    return saveToFile();
  }

  if (it != entries.end()) {
    if (it->tokens.accessToken == tokens.accessToken && it->tokens.refreshToken == tokens.refreshToken &&
        it->tokens.refreshUrl == tokens.refreshUrl) {
      return true;  // unchanged: skip the SD write
    }
    it->tokens = tokens;
    return saveToFile();
  }

  if (entries.size() >= MAX_SERVERS) {
    // Full: evict the oldest entry so a new server can still be remembered.
    entries.erase(entries.begin());
  }
  entries.push_back(Entry{url, tokens});
  return saveToFile();
}
