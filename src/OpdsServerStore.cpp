#include "OpdsServerStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>

void OpdsServerStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
    JsonArray headers = obj["headers"].to<JsonArray>();
    for (const auto& header : server.customHeaders) {
      JsonObject headerObj = headers.add<JsonObject>();
      headerObj["name"] = header.name;
      headerObj["value_obf"] = obfuscation::obfuscateToBase64(header.value);
    }
  }
}

bool OpdsServerStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'servers' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  servers.clear();
  JsonArrayConst arr = doc["servers"].as<JsonArrayConst>();
  servers.reserve(std::min(arr.size(), MAX_SERVERS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | "";
    server.url = obj["url"] | "";
    server.username = obj["username"] | "";
    server.password = extractPassword(obj, needsResave);

    size_t headerIndex = 0;
    for (JsonObjectConst headerObj : obj["headers"].as<JsonArrayConst>()) {
      if (headerIndex >= OpdsServer::MAX_CUSTOM_HEADERS) break;
      server.customHeaders[headerIndex].name = headerObj["name"] | "";
      server.customHeaders[headerIndex].value = obfuscation::deobfuscateFromBase64(headerObj["value_obf"] | "");
      headerIndex++;
    }

    servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", servers.size());

  if (needsResave) {
    LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
    requestResave();
  }

  return true;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  servers.push_back(server);
  LOG_DBG("OPS", "Added server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (index >= servers.size()) {
    return false;
  }

  servers[index] = server;
  LOG_DBG("OPS", "Updated server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }

  LOG_DBG("OPS", "Removed server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}

std::vector<HttpHeader> OpdsServer::activeCustomHeaders() const {
  std::vector<HttpHeader> active;
  active.reserve(MAX_CUSTOM_HEADERS);
  for (const auto& header : customHeaders) {
    if (!header.name.empty()) active.push_back(header);
  }
  return active;
}
