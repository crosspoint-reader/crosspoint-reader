#include "PasswordStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

PasswordStore PasswordStore::instance;

namespace {
constexpr char PASSWORD_FILE[] = "/.crosspoint/passwords.json";
}

bool PasswordStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["site"] = entry.site;
    obj["username_obf"] = obfuscation::obfuscateToBase64(entry.username);
    obj["password_obf"] = obfuscation::obfuscateToBase64(entry.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(PASSWORD_FILE, json);
}

bool PasswordStore::loadFromFile() {
  if (!Storage.exists(PASSWORD_FILE)) {
    return false;
  }

  String json = Storage.readFile(PASSWORD_FILE);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PWS", "JSON parse error: %s", error.c_str());
    return false;
  }

  entries.clear();
  JsonArray arr = doc["entries"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (entries.size() >= MAX_ENTRIES) break;
    PasswordEntry entry;
    entry.site = obj["site"] | std::string("");

    bool ok = false;
    entry.username = obfuscation::deobfuscateFromBase64(obj["username_obf"] | "", &ok);
    if (!ok) entry.username = obj["username"] | std::string("");

    ok = false;
    entry.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok) entry.password = obj["password"] | std::string("");

    entries.push_back(entry);
  }

  LOG_DBG("PWS", "Loaded %zu password entries", entries.size());
  return true;
}

bool PasswordStore::addEntry(const std::string& site, const std::string& username, const std::string& password) {
  if (entries.size() >= MAX_ENTRIES) {
    return false;
  }
  entries.push_back({site, username, password});
  return saveToFile();
}

bool PasswordStore::removeEntry(size_t index) {
  if (index >= entries.size()) {
    return false;
  }
  entries.erase(entries.begin() + index);
  return saveToFile();
}
