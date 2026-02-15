#include "KOReaderCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Serialization.h>

// Initialize the static instance
KOReaderCredentialStore KOReaderCredentialStore::instance;

namespace {
// File format version
constexpr uint8_t KOREADER_FILE_VERSION = 1;

// KOReader credentials file path
constexpr char KOREADER_FILE[] = "/.crosspoint/koreader.bin";

// Default sync server URL
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";

// Obfuscation key - "KOReader" in ASCII
// This is NOT cryptographic security, just prevents casual file reading
constexpr uint8_t OBFUSCATION_KEY[] = {0x4B, 0x4F, 0x52, 0x65, 0x61, 0x64, 0x65, 0x72};
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);
}  // namespace

void KOReaderCredentialStore::obfuscate(std::string& data) const {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

bool KOReaderCredentialStore::saveToFile() const {
  // Make sure the directory exists
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("KRS", KOREADER_FILE, file)) {
    return false;
  }

  // Write header
  serialization::writePod(file, KOREADER_FILE_VERSION);

  // Write username (plaintext - not particularly sensitive)
  serialization::writeString(file, username);
  LOG_DBG("KRS", "Saving username: %s", username.c_str());

  // Write password (obfuscated)
  std::string obfuscatedPwd = password;
  obfuscate(obfuscatedPwd);
  serialization::writeString(file, obfuscatedPwd);

  // Write server URL
  serialization::writeString(file, serverUrl);

  // Write match method
  serialization::writePod(file, static_cast<uint8_t>(matchMethod));

  file.close();
  LOG_DBG("KRS", "Saved KOReader credentials to file");
  return true;
}

bool KOReaderCredentialStore::loadFromFile() {
  FsFile file;
  if (!Storage.openFileForRead("KRS", KOREADER_FILE, file)) {
    LOG_DBG("KRS", "No credentials file found");
    return false;
  }

  // Read and verify version
  uint8_t version;
  serialization::readPod(file, version);
  if (version != KOREADER_FILE_VERSION) {
    LOG_DBG("KRS", "Unknown file version: %u", version);
    file.close();
    return false;
  }

  // Read username
  if (file.available()) {
    serialization::readString(file, username);
  } else {
    username.clear();
  }

  // Read and deobfuscate password
  if (file.available()) {
    serialization::readString(file, password);
    obfuscate(password);  // XOR is symmetric, so same function deobfuscates
  } else {
    password.clear();
  }

  // Read server URL
  if (file.available()) {
    serialization::readString(file, serverUrl);
  } else {
    serverUrl.clear();
  }

  // Read match method
  if (file.available()) {
    uint8_t method;
    serialization::readPod(file, method);
    matchMethod = static_cast<DocumentMatchMethod>(method);
  } else {
    matchMethod = DocumentMatchMethod::FILENAME;
  }

  file.close();
  LOG_DBG("KRS", "Loaded KOReader credentials for user: %s", username.c_str());
  return true;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }

  // Calculate MD5 hash of password using ESP32's MD5Builder
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("KRS", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  if (serverUrl.empty()) {
    return DEFAULT_SERVER_URL;
  }

  // Normalize URL: add http:// if no protocol specified (local servers typically don't have SSL)
  if (serverUrl.find("://") == std::string::npos) {
    return "http://" + serverUrl;
  }

  return serverUrl;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}
