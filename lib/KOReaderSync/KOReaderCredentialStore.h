#pragma once
#include <string>

/**
 * Singleton class for storing KOReader sync credentials on the SD card.
 * Credentials are stored in /sd/.crosspoint/koreader.bin with basic
 * XOR obfuscation to prevent casual reading (not cryptographically secure).
 */
class KOReaderCredentialStore {
 private:
  static KOReaderCredentialStore instance;
  std::string username;
  std::string password;

  // Private constructor for singleton
  KOReaderCredentialStore() = default;

  // XOR obfuscation (symmetric - same for encode/decode)
  void obfuscate(std::string& data) const;

 public:
  // Delete copy constructor and assignment
  KOReaderCredentialStore(const KOReaderCredentialStore&) = delete;
  KOReaderCredentialStore& operator=(const KOReaderCredentialStore&) = delete;

  // Get singleton instance
  static KOReaderCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  // Get MD5 hash of password for API authentication
  std::string getMd5Password() const;

  // Check if credentials are set
  bool hasCredentials() const;

  // Clear credentials
  void clearCredentials();
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
