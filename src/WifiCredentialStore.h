#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
  // Connection hint cached on successful connect. All-zero BSSID or channel==0 means
  // "no hint, do a full scan". Used to skip channel scanning on reconnect.
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  uint8_t channel = 0;

  // Cached IP configuration to skip DHCP on reconnect. Valid only when ip[0] != 0 AND
  // we connect to the same BSSID (cached above). cacheTimestamp is epoch seconds at
  // capture; 0 means "unknown time, no TTL enforced".
  uint8_t ip[4] = {0, 0, 0, 0};
  uint8_t gateway[4] = {0, 0, 0, 0};
  uint8_t mask[4] = {0, 0, 0, 0};
  uint8_t dns[4] = {0, 0, 0, 0};
  uint32_t cacheTimestamp = 0;
};

class WifiCredentialStore;
namespace JsonSettingsIO {
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing WiFi credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */
class WifiCredentialStore {
 private:
  static WifiCredentialStore instance;
  std::vector<WifiCredential> credentials;
  std::string lastConnectedSsid;

  static constexpr size_t MAX_NETWORKS = 8;

  // Private constructor for singleton
  WifiCredentialStore() = default;

  bool loadFromBinaryFile();

  friend bool JsonSettingsIO::saveWifi(const WifiCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadWifi(WifiCredentialStore&, const char*, bool*);

 public:
  // Delete copy constructor and assignment
  WifiCredentialStore(const WifiCredentialStore&) = delete;
  WifiCredentialStore& operator=(const WifiCredentialStore&) = delete;

  // Get singleton instance
  static WifiCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();

  // Credential management
  bool addCredential(const std::string& ssid, const std::string& password);
  bool removeCredential(const std::string& ssid);
  const WifiCredential* findCredential(const std::string& ssid) const;

  // Update cached BSSID/channel hint AND IP configuration in one write. ip/gw/mask/dns
  // may be all-zero to mean "no IP cache". cacheTimestamp is epoch seconds (0 if unsynced).
  // Persists only if anything changed.
  bool updateConnectionCache(const std::string& ssid, const uint8_t bssid[6], uint8_t channel, const uint8_t ip[4],
                             const uint8_t gateway[4], const uint8_t mask[4], const uint8_t dns[4],
                             uint32_t cacheTimestamp);
  // Clear all cached hint + IP for this credential (after a hint-based connect failed
  // and we want the next attempt to start fresh with full scan + DHCP).
  bool clearConnectionCache(const std::string& ssid);

  // Get all stored credentials (for UI display)
  const std::vector<WifiCredential>& getCredentials() const { return credentials; }

  // Check if a network is saved
  bool hasSavedCredential(const std::string& ssid) const;

  // Last connected network
  void setLastConnectedSsid(const std::string& ssid);
  const std::string& getLastConnectedSsid() const;
  void clearLastConnectedSsid();

  // Clear all credentials
  void clearAll();
};

// Helper macro to access credentials store
#define WIFI_STORE WifiCredentialStore::getInstance()
