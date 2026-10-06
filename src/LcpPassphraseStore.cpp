#include "LcpPassphraseStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>

namespace {
// A hex sha256 is exactly 64 chars; anything longer is a malformed file.
constexpr size_t MAX_HASH_CHARS = 64;
}  // namespace

void LcpPassphraseStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["passphrases"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["provider"] = entry.provider;
    obj["key_obf"] = obfuscation::obfuscateToBase64(entry.userKeyHex);
  }
}

bool LcpPassphraseStore::fromJson(JsonVariantConst doc) {
  entries.clear();
  JsonArrayConst arr = doc["passphrases"].as<JsonArrayConst>();
  entries.reserve(std::min(arr.size(), MAX_PROVIDERS));
  for (JsonObjectConst obj : arr) {
    if (entries.size() >= MAX_PROVIDERS) break;
    Entry entry;
    entry.provider = obj["provider"] | "";
    if (entry.provider.empty()) continue;
    bool ok = false;
    bool tooLong = false;
    entry.userKeyHex = obfuscation::deobfuscateFromBase64(obj["key_obf"] | "", MAX_HASH_CHARS, &ok, &tooLong);
    if (entry.userKeyHex.empty()) continue;
    entries.push_back(std::move(entry));
  }
  LOG_DBG("LCPPASS", "Loaded %zu LCP passphrase entries", entries.size());
  return true;
}

std::string LcpPassphraseStore::get(const std::string& provider) const {
  for (const auto& entry : entries) {
    if (entry.provider == provider) return entry.userKeyHex;
  }
  return {};
}

bool LcpPassphraseStore::put(const std::string& provider, const std::string& userKeyHex) {
  const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.provider == provider; });

  if (userKeyHex.empty()) {
    if (it == entries.end()) return true;
    entries.erase(it);
    return saveToFile();
  }

  if (it != entries.end()) {
    if (it->userKeyHex == userKeyHex) return true;  // unchanged: skip the SD write
    it->userKeyHex = userKeyHex;
    return saveToFile();
  }

  if (entries.size() >= MAX_PROVIDERS) {
    // Full: evict the oldest entry so a new library can still be remembered.
    entries.erase(entries.begin());
  }
  entries.push_back(Entry{provider, userKeyHex});
  return saveToFile();
}
