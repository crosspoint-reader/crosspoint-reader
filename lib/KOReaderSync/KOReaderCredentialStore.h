#pragma once
#include <cstdint>
#include <string>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

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
  std::string serverUrl;                                            // Custom sync server URL (empty = default)
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility

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

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  // Get base URL for API calls (with http:// normalization if no protocol, falls back to default)
  std::string getBaseUrl() const;

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
