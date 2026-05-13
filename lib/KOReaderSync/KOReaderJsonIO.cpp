#include "KOReaderJsonIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "KOReaderCredentialStore.h"

namespace KOReaderJsonIO {

bool save(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["username"] = store.getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.getPassword());
  doc["serverUrl"] = store.getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(store.getMatchMethod());

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool load(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  std::string user = doc["username"] | std::string("");

  bool ok = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || pass.empty()) {
    pass = doc["password"] | std::string("");
    if (!pass.empty() && needsResave) *needsResave = true;
  }

  store.setCredentials(user, pass);
  store.setServerUrl(doc["serverUrl"] | std::string(""));

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.setMatchMethod(static_cast<DocumentMatchMethod>(method));

  return true;
}

}  // namespace KOReaderJsonIO
