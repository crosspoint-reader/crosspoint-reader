#include "WifiCredentialStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include <cstring>

// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
// File format version (for binary migration)
constexpr uint8_t WIFI_FILE_VERSION = 2;

// File paths
constexpr char WIFI_FILE_BIN[] = "/.crosspoint/wifi.bin";
constexpr char WIFI_FILE_JSON[] = "/.crosspoint/wifi.json";
constexpr char WIFI_FILE_BAK[] = "/.crosspoint/wifi.bin.bak";

// Legacy obfuscation key - "CrossPoint" in ASCII (only used for binary migration)
constexpr uint8_t LEGACY_OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t LEGACY_KEY_LENGTH = sizeof(LEGACY_OBFUSCATION_KEY);

void legacyDeobfuscate(std::string& data) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= LEGACY_OBFUSCATION_KEY[i % LEGACY_KEY_LENGTH];
  }
}
}  // namespace

bool WifiCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveWifi(*this, WIFI_FILE_JSON);
}

bool WifiCredentialStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(WIFI_FILE_JSON)) {
    String json = Storage.readFile(WIFI_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadWifi(*this, json.c_str(), &resave);
      if (result && resave) {
        LOG_DBG("WCS", "Resaving JSON with obfuscated passwords");
        saveToFile();
      }
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(WIFI_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(WIFI_FILE_BIN, WIFI_FILE_BAK);
        LOG_DBG("WCS", "Migrated wifi.bin to wifi.json");
        return true;
      } else {
        LOG_ERR("WCS", "Failed to save wifi during migration");
        return false;
      }
    }
  }

  return false;
}

bool WifiCredentialStore::loadFromBinaryFile() {
  FsFile file;
  if (!Storage.openFileForRead("WCS", WIFI_FILE_BIN, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version > WIFI_FILE_VERSION) {
    LOG_DBG("WCS", "Unknown file version: %u", version);
    return false;
  }

  if (version >= 2) {
    serialization::readString(file, lastConnectedSsid);
  } else {
    lastConnectedSsid.clear();
  }

  uint8_t count;
  serialization::readPod(file, count);

  credentials.clear();
  for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
    WifiCredential cred;
    serialization::readString(file, cred.ssid);
    serialization::readString(file, cred.password);
    legacyDeobfuscate(cred.password);
    credentials.push_back(cred);
  }

  // LOG_DBG("WCS", "Loaded %zu WiFi credentials from binary file", credentials.size());
  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  // Check if this SSID already exists and update it
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != credentials.end()) {
    cred->password = password;
    LOG_DBG("WCS", "Updated credentials for: %s", ssid.c_str());
    return saveToFile();
  }

  // Check if we've reached the limit
  if (credentials.size() >= MAX_NETWORKS) {
    LOG_DBG("WCS", "Cannot add more networks, limit of %zu reached", MAX_NETWORKS);
    return false;
  }

  // Add new credential
  credentials.push_back({ssid, password});
  LOG_DBG("WCS", "Added credentials for: %s", ssid.c_str());
  return saveToFile();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != credentials.end()) {
    credentials.erase(cred);
    LOG_DBG("WCS", "Removed credentials for: %s", ssid.c_str());
    if (ssid == lastConnectedSsid) {
      clearLastConnectedSsid();
    }
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

bool WifiCredentialStore::updateConnectionCache(const std::string& ssid, const uint8_t bssid[6], uint8_t channel,
                                                const uint8_t ip[4], const uint8_t gateway[4], const uint8_t mask[4],
                                                const uint8_t dns[4], uint32_t cacheTimestamp) {
  const auto cred =
      find_if(credentials.begin(), credentials.end(), [&ssid](const WifiCredential& c) { return c.ssid == ssid; });
  if (cred == credentials.end()) {
    return false;
  }
  const bool sameHint = (channel == cred->channel) && std::memcmp(bssid, cred->bssid, 6) == 0;
  const bool sameIp = std::memcmp(ip, cred->ip, 4) == 0 && std::memcmp(gateway, cred->gateway, 4) == 0 &&
                      std::memcmp(mask, cred->mask, 4) == 0 && std::memcmp(dns, cred->dns, 4) == 0;
  // Timestamp updates are not worth a write on their own; only persist when topology changed.
  if (sameHint && sameIp) {
    return true;
  }
  std::memcpy(cred->bssid, bssid, 6);
  cred->channel = channel;
  std::memcpy(cred->ip, ip, 4);
  std::memcpy(cred->gateway, gateway, 4);
  std::memcpy(cred->mask, mask, 4);
  std::memcpy(cred->dns, dns, 4);
  cred->cacheTimestamp = cacheTimestamp;
  return saveToFile();
}

bool WifiCredentialStore::clearConnectionCache(const std::string& ssid) {
  const auto cred =
      find_if(credentials.begin(), credentials.end(), [&ssid](const WifiCredential& c) { return c.ssid == ssid; });
  if (cred == credentials.end()) {
    return false;
  }
  bool empty = (cred->channel == 0) && (cred->ip[0] == 0);
  for (int i = 0; i < 6 && empty; i++) {
    if (cred->bssid[i] != 0) empty = false;
  }
  if (empty) {
    return true;
  }
  std::memset(cred->bssid, 0, 6);
  cred->channel = 0;
  std::memset(cred->ip, 0, 4);
  std::memset(cred->gateway, 0, 4);
  std::memset(cred->mask, 0, 4);
  std::memset(cred->dns, 0, 4);
  cred->cacheTimestamp = 0;
  return saveToFile();
}

void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) {
  if (lastConnectedSsid != ssid) {
    lastConnectedSsid = ssid;
    saveToFile();
  }
}

const std::string& WifiCredentialStore::getLastConnectedSsid() const { return lastConnectedSsid; }

void WifiCredentialStore::clearLastConnectedSsid() {
  if (!lastConnectedSsid.empty()) {
    lastConnectedSsid.clear();
    saveToFile();
  }
}

void WifiCredentialStore::clearAll() {
  credentials.clear();
  lastConnectedSsid.clear();
  saveToFile();
  LOG_DBG("WCS", "Cleared all WiFi credentials");
}
