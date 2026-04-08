#pragma once
#include <string>

class HardcoverCredentialStore;
namespace JsonSettingsIO {
bool saveHardcover(const HardcoverCredentialStore& store, const char* path);
bool loadHardcover(HardcoverCredentialStore& store, const char* json);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing Hardcover.app API token on the SD card.
 *
 * The bearer token is obfuscated with the device MAC address and
 * base64-encoded before writing to JSON (same approach as
 * KOReaderCredentialStore — not cryptographically secure, but prevents
 * casual reading and ties credentials to the specific device).
 */
class HardcoverCredentialStore {
 private:
  static HardcoverCredentialStore instance;
  std::string token;  // Bearer token from Hardcover account settings

  // Private constructor for singleton
  HardcoverCredentialStore() = default;

  friend bool JsonSettingsIO::saveHardcover(const HardcoverCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadHardcover(HardcoverCredentialStore&, const char*);

 public:
  // Delete copy constructor and assignment
  HardcoverCredentialStore(const HardcoverCredentialStore&) = delete;
  HardcoverCredentialStore& operator=(const HardcoverCredentialStore&) = delete;

  // Get singleton instance
  static HardcoverCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();

  // Token management
  void setToken(const std::string& bearerToken);
  const std::string& getToken() const { return token; }

  // Check if token is set
  bool hasToken() const;

  // Clear token
  void clearToken();
};

// Helper macro to access credential store
#define HARDCOVER_STORE HardcoverCredentialStore::getInstance()
