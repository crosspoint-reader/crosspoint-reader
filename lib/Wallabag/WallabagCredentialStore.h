#pragma once
#include <cstdint>
#include <string>

/**
 * Singleton class for storing Wallabag credentials on the SD card.
 * Credentials are stored in /sd/.crosspoint/wallabag.bin with basic
 * XOR obfuscation to prevent casual reading (not cryptographically secure).
 */
class WallabagCredentialStore {
 private:
  static WallabagCredentialStore instance;
  std::string serverUrl;
  std::string clientId;
  std::string clientSecret;
  std::string username;
  std::string password;
  std::string accessToken;
  int64_t tokenExpiry = 0;  // Unix timestamp when token expires
  uint8_t articleLimit = 30;

  // Private constructor for singleton
  WallabagCredentialStore() = default;

  // XOR obfuscation (symmetric - same for encode/decode)
  void obfuscate(std::string& data) const;

 public:
  // Delete copy constructor and assignment
  WallabagCredentialStore(const WallabagCredentialStore&) = delete;
  WallabagCredentialStore& operator=(const WallabagCredentialStore&) = delete;

  // Get singleton instance
  static WallabagCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();

  // Server URL
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  // OAuth2 credentials
  void setClientId(const std::string& id);
  const std::string& getClientId() const { return clientId; }

  void setClientSecret(const std::string& secret);
  const std::string& getClientSecret() const { return clientSecret; }

  void setUsername(const std::string& user);
  const std::string& getUsername() const { return username; }

  void setPassword(const std::string& pass);
  const std::string& getPassword() const { return password; }

  // Article limit (0 = unlimited)
  void setArticleLimit(uint8_t limit);
  uint8_t getArticleLimit() const { return articleLimit; }

  // Check if enough credentials are configured to attempt authentication
  bool hasCredentials() const;

  // Token management
  void storeToken(const std::string& token, int64_t expiresIn);
  bool isTokenValid() const;
  const std::string& getAccessToken() const { return accessToken; }
  void clearToken();
};

// Helper macro to access credential store
#define WALLABAG_STORE WallabagCredentialStore::getInstance()
