#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

/**
 * Persists the CrossInk API URL, device ID and auth token on SD card.
 * Stored at /.crosspoint/substack.json
 */
class SubstackStore : public PersistableStore<SubstackStore> {
 private:
  SubstackStore() = default;
  friend class PersistableStore<SubstackStore>;

 public:
  // CrossInk API base URL, e.g. "http://192.168.1.100:3000"
  char apiUrl[128] = "";
  // JWT token issued by CrossInk API (Bearer token)
  char apiToken[256] = "";
  // Device UUID assigned by CrossInk API
  char deviceId[64] = "";

  static const char* getFilePath() { return "/.crosspoint/substack.json"; }

  bool isConfigured() const { return apiUrl[0] != '\0' && deviceId[0] != '\0'; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};

#define SUBSTACK_STORE SubstackStore::getInstance()
