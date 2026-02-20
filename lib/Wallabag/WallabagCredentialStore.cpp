#include "WallabagCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <ctime>

// Initialize the static instance
WallabagCredentialStore WallabagCredentialStore::instance;

namespace {
// File format version
constexpr uint8_t WALLABAG_FILE_VERSION = 1;

// Wallabag credentials file path
constexpr char WALLABAG_FILE[] = "/.crosspoint/wallabag.bin";

// Token validity buffer: expire 5 minutes early to avoid edge cases
constexpr int64_t TOKEN_EXPIRY_BUFFER_SEC = 300;

// Obfuscation key - "Wallabag" in ASCII
// This is NOT cryptographic security, just prevents casual file reading
constexpr uint8_t OBFUSCATION_KEY[] = {0x57, 0x61, 0x6C, 0x6C, 0x61, 0x62, 0x61, 0x67};
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);
}  // namespace

void WallabagCredentialStore::obfuscate(std::string& data) const {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

bool WallabagCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("WBG", WALLABAG_FILE, file)) {
    return false;
  }

  serialization::writePod(file, WALLABAG_FILE_VERSION);
  serialization::writeString(file, serverUrl);
  serialization::writeString(file, clientId);

  std::string obfuscatedSecret = clientSecret;
  obfuscate(obfuscatedSecret);
  serialization::writeString(file, obfuscatedSecret);

  serialization::writeString(file, username);

  std::string obfuscatedPwd = password;
  obfuscate(obfuscatedPwd);
  serialization::writeString(file, obfuscatedPwd);

  std::string obfuscatedToken = accessToken;
  obfuscate(obfuscatedToken);
  serialization::writeString(file, obfuscatedToken);

  serialization::writePod(file, tokenExpiry);
  serialization::writePod(file, articleLimit);

  file.close();
  LOG_DBG("WBG", "Saved Wallabag credentials to file");
  return true;
}

bool WallabagCredentialStore::loadFromFile() {
  FsFile file;
  if (!Storage.openFileForRead("WBG", WALLABAG_FILE, file)) {
    LOG_DBG("WBG", "No credentials file found");
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != WALLABAG_FILE_VERSION) {
    LOG_DBG("WBG", "Unknown file version: %u", version);
    file.close();
    return false;
  }

  if (file.available()) serialization::readString(file, serverUrl);
  if (file.available()) serialization::readString(file, clientId);

  if (file.available()) {
    serialization::readString(file, clientSecret);
    obfuscate(clientSecret);
  }

  if (file.available()) serialization::readString(file, username);

  if (file.available()) {
    serialization::readString(file, password);
    obfuscate(password);
  }

  if (file.available()) {
    serialization::readString(file, accessToken);
    obfuscate(accessToken);
  }

  if (file.available()) serialization::readPod(file, tokenExpiry);
  if (file.available()) serialization::readPod(file, articleLimit);

  file.close();
  LOG_DBG("WBG", "Loaded Wallabag credentials, server: %s", serverUrl.c_str());
  return true;
}

void WallabagCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("WBG", "Set server URL: %s", url.c_str());
}

void WallabagCredentialStore::setClientId(const std::string& id) { clientId = id; }

void WallabagCredentialStore::setClientSecret(const std::string& secret) { clientSecret = secret; }

void WallabagCredentialStore::setUsername(const std::string& user) { username = user; }

void WallabagCredentialStore::setPassword(const std::string& pass) { password = pass; }

void WallabagCredentialStore::setArticleLimit(uint8_t limit) { articleLimit = limit; }

bool WallabagCredentialStore::hasCredentials() const {
  return !serverUrl.empty() && !clientId.empty() && !clientSecret.empty() && !username.empty() && !password.empty();
}

void WallabagCredentialStore::storeToken(const std::string& token, int64_t expiresIn) {
  accessToken = token;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  tokenExpiry = now + expiresIn - TOKEN_EXPIRY_BUFFER_SEC;
  LOG_DBG("WBG", "Stored access token, expires in %lld sec", (long long)(expiresIn - TOKEN_EXPIRY_BUFFER_SEC));
}

bool WallabagCredentialStore::isTokenValid() const {
  if (accessToken.empty()) return false;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  return now < tokenExpiry;
}

void WallabagCredentialStore::clearToken() {
  accessToken.clear();
  tokenExpiry = 0;
}
