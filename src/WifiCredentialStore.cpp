#include "WifiCredentialStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
constexpr char WIFI_FILE_JSON[] = "/.crosspoint/wifi.json";
}  // namespace

bool WifiCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveWifi(*this, WIFI_FILE_JSON);
}

bool WifiCredentialStore::loadFromFile() {
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
  return false;
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
  if (sameHint && sameIp) {
    // Refresh the TTL only if the cache is older than half the TTL window — otherwise
    // a long-running device that reconnects daily would let its 7-day TTL expire even
    // though the topology hasn't changed. Half-TTL avoids an SD write on every connect.
    constexpr uint32_t REFRESH_THRESHOLD_SECONDS = (7 * 24 * 60 * 60) / 2;
    const int64_t elapsed = static_cast<int64_t>(cacheTimestamp) - static_cast<int64_t>(cred->cacheTimestamp);
    if (cacheTimestamp != 0 && cred->cacheTimestamp != 0 && elapsed > REFRESH_THRESHOLD_SECONDS) {
      cred->cacheTimestamp = cacheTimestamp;
      return saveToFile();
    }
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

void WifiCredentialStore::setLastKnownMacAddress(const std::string& mac) {
  if (lastKnownMacAddress != mac) {
    lastKnownMacAddress = mac;
    saveToFile();
  }
}

const std::string& WifiCredentialStore::getLastKnownMacAddress() const { return lastKnownMacAddress; }

void WifiCredentialStore::clearAll() {
  credentials.clear();
  lastConnectedSsid.clear();
  lastKnownMacAddress.clear();
  saveToFile();
  LOG_DBG("WCS", "Cleared all WiFi credentials");
}
