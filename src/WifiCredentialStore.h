#pragma once
#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
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
