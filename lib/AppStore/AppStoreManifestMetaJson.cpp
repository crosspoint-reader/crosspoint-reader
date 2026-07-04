#include "AppStoreManifestMetaJson.h"

#include <ArduinoJson.h>

namespace AppStoreManifestMetaJson {

bool parse(const char* json, AppStoreManifestMetaData& out) {
  out = {};
  if (json == nullptr || json[0] == '\0') {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    return false;
  }

  out.fetchedAt = doc["fetched_at"] | "";
  out.appCount = doc["app_count"] | 0u;
  return !out.fetchedAt.empty();
}

std::string serialize(const AppStoreManifestMetaData& meta) {
  JsonDocument doc;
  doc["fetched_at"] = meta.fetchedAt;
  doc["app_count"] = meta.appCount;
  std::string json;
  serializeJson(doc, json);
  return json;
}

}  // namespace AppStoreManifestMetaJson
