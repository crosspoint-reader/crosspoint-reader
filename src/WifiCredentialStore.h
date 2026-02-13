#pragma once
#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;  // Stored obfuscated in file
};

/**
 * Singleton class for storing WiFi credentials on the SD card.
 * Credentials are stored in /sd/.crosspoint/wifi.bin with basic
 * XOR obfuscation to prevent casual reading (not cryptographically secure).
 */
class WifiCredentialStore {
 private:
  static WifiCredentialStore instance;
  std::vector<WifiCredential> credentials;

  static constexpr size_t MAX_NETWORKS = 8;

  // Private constructor for singleton
  WifiCredentialStore() = default;

  // XOR obfuscation (symmetric - same for encode/decode)
  void obfuscate(std::string& data) const;

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

  // Clear all credentials
  void clearAll();
};

// Helper macro to access credentials store
#define WIFI_STORE WifiCredentialStore::getInstance()
