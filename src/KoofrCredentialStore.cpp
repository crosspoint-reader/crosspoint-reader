#include "KoofrCredentialStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
// Koofr's WebDAV endpoint. "Koofr" is the name of the default place (the
// personal storage root); the path is fixed for every account.
constexpr char DEFAULT_SERVER_URL[] = "https://app.koofr.net/dav/Koofr";

// Where highlights land when the user hasn't picked a folder.
constexpr char DEFAULT_REMOTE_DIR[] = "CrossPoint/Highlights";

// Strip the slashes that separate this component from its neighbours, so
// callers can join with a single '/' without producing "//".
void trimSlashes(std::string& value) {
  size_t begin = 0;
  while (begin < value.size() && value[begin] == '/') ++begin;
  size_t end = value.size();
  while (end > begin && value[end - 1] == '/') --end;
  value = value.substr(begin, end - begin);
}
}  // namespace

void KoofrCredentialStore::toJson(JsonDocument& doc) const {
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
  doc["remoteDir"] = getRemoteDir();
}

bool KoofrCredentialStore::fromJson(JsonVariantConst doc) {
  const std::string user = doc["username"] | "";

  bool needsResave = false;
  const std::string pass = extractPassword(doc, needsResave);

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");
  setRemoteDir(doc["remoteDir"] | "");

  if (needsResave) {
    LOG_DBG("KOOFR", "Resaving Koofr credentials to update format");
    requestResave();
  }

  return true;
}

void KoofrCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("KOOFR", "Set credentials for user: %s", user.c_str());
}

bool KoofrCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KoofrCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("KOOFR", "Cleared Koofr credentials");
}

void KoofrCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("KOOFR", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KoofrCredentialStore::getBaseUrl() const {
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else if (serverUrl.find("://") == std::string::npos) {
    // Normalize: a bare host is assumed to be a self-hosted WebDAV server,
    // which typically has no TLS in front of it.
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }

  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

void KoofrCredentialStore::setRemoteDir(const std::string& dir) {
  remoteDir = dir;
  trimSlashes(remoteDir);
  LOG_DBG("KOOFR", "Set remote dir: %s", remoteDir.empty() ? "(default)" : remoteDir.c_str());
}

std::string KoofrCredentialStore::getEffectiveRemoteDir() const {
  return remoteDir.empty() ? DEFAULT_REMOTE_DIR : remoteDir;
}
