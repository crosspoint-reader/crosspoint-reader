#include "SubstackStore.h"

void SubstackStore::toJson(JsonDocument& doc) const {
  doc["apiUrl"] = apiUrl;
  doc["apiToken"] = apiToken;
  doc["deviceId"] = deviceId;
}

bool SubstackStore::fromJson(JsonVariantConst doc) {
  if (doc["apiUrl"].is<const char*>()) {
    snprintf(apiUrl, sizeof(apiUrl), "%s", doc["apiUrl"].as<const char*>());
  }
  if (doc["apiToken"].is<const char*>()) {
    snprintf(apiToken, sizeof(apiToken), "%s", doc["apiToken"].as<const char*>());
  }
  if (doc["deviceId"].is<const char*>()) {
    snprintf(deviceId, sizeof(deviceId), "%s", doc["deviceId"].as<const char*>());
  }
  return true;
}
