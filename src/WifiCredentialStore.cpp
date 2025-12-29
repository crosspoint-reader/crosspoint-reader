#include "WifiCredentialStore.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
// File format version
constexpr uint8_t WIFI_FILE_VERSION = 1;

// WiFi credentials file path
constexpr char WIFI_FILE[] = "/.crosspoint/wifi.bin";

// Obfuscation key - "CrossPoint" in ASCII
// This is NOT cryptographic security, just prevents casual file reading
constexpr uint8_t OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);
}  // namespace

void WifiCredentialStore::obfuscate(std::string& data) const {
  Serial.printf("[%lu] [WCS] Obfuscating/deobfuscating %zu bytes\n", millis(), data.size());
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

bool WifiCredentialStore::saveToFile() const {
  // Make sure the directory exists
  SdMan.mkdir("/.crosspoint");

  FsFile file;
  if (!SdMan.openFileForWrite("WCS", WIFI_FILE, file)) {
    return false;
  }

  // Write header
  serialization::writePod(file, WIFI_FILE_VERSION);
  serialization::writePod(file, static_cast<uint8_t>(credentials.size()));

  // Write each credential
  for (const auto& cred : credentials) {
    // Write SSID (plaintext - not sensitive)
    serialization::writeString(file, cred.ssid);
    Serial.printf("[%lu] [WCS] Saving SSID: %s, password length: %zu\n", millis(), cred.ssid.c_str(),
                  cred.password.size());

    // Write password (obfuscated)
    std::string obfuscatedPwd = cred.password;
    obfuscate(obfuscatedPwd);
    serialization::writeString(file, obfuscatedPwd);
  }

  file.close();
  Serial.printf("[%lu] [WCS] Saved %zu WiFi credentials to file\n", millis(), credentials.size());
  return true;
}

bool WifiCredentialStore::loadFromFile() {
  FsFile file;
  if (!SdMan.openFileForRead("WCS", WIFI_FILE, file)) {
    return false;
  }

  // Read and verify version
  uint8_t version;
  serialization::readPod(file, version);
  if (version != WIFI_FILE_VERSION) {
    Serial.printf("[%lu] [WCS] Unknown file version: %u\n", millis(), version);
    file.close();
    return false;
  }

  // Read credential count
  uint8_t count;
  serialization::readPod(file, count);

  // Read credentials
  credentials.clear();
  for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
    WifiCredential cred;

    // Read SSID
    serialization::readString(file, cred.ssid);

    // Read and deobfuscate password
    serialization::readString(file, cred.password);
    Serial.printf("[%lu] [WCS] Loaded SSID: %s, obfuscated password length: %zu\n", millis(), cred.ssid.c_str(),
                  cred.password.size());
    obfuscate(cred.password);  // XOR is symmetric, so same function deobfuscates
    Serial.printf("[%lu] [WCS] After deobfuscation, password length: %zu\n", millis(), cred.password.size());

    credentials.push_back(cred);
  }

  file.close();
  Serial.printf("[%lu] [WCS] Loaded %zu WiFi credentials from file\n", millis(), credentials.size());
  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  // Check if this SSID already exists and update it
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != credentials.end()) {
    cred->password = password;
    Serial.printf("[%lu] [WCS] Updated credentials for: %s\n", millis(), ssid.c_str());
    return saveToFile();
  }

  // Check if we've reached the limit
  if (credentials.size() >= MAX_NETWORKS) {
    Serial.printf("[%lu] [WCS] Cannot add more networks, limit of %zu reached\n", millis(), MAX_NETWORKS);
    return false;
  }

  // Add new credential
  credentials.push_back({ssid, password});
  Serial.printf("[%lu] [WCS] Added credentials for: %s\n", millis(), ssid.c_str());
  return saveToFile();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != credentials.end()) {
    credentials.erase(cred);
    Serial.printf("[%lu] [WCS] Removed credentials for: %s\n", millis(), ssid.c_str());
    return saveToFile();
  }
  return false;  // Not found
}

const WifiCredential* WifiCredentialStore::findCredential(const std::string& ssid) const {
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });

  if (cred != credentials.end()) {
    return &*cred;
  }

  return nullptr;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const { return findCredential(ssid) != nullptr; }

void WifiCredentialStore::clearAll() {
  credentials.clear();
  saveToFile();
  Serial.printf("[%lu] [WCS] Cleared all WiFi credentials\n", millis());
}
