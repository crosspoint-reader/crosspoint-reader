#pragma once
#include <string>

class ReadestCredentialStore;
namespace JsonSettingsIO {
bool saveReadest(const ReadestCredentialStore& store, const char* path);
bool loadReadest(ReadestCredentialStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton for storing Readest sync credentials on the SD card.
 * Uses the KOReader sync protocol — requires a user-deployed KOReader-compatible
 * sync server (e.g. koreader-sync-server). Readest does not host a server.
 *
 * Passwords are XOR-obfuscated with the device MAC address before writing.
 */
class ReadestCredentialStore {
 private:
  static ReadestCredentialStore instance;
  std::string username;
  std::string password;
  std::string serverUrl;

  ReadestCredentialStore() = default;

  friend bool JsonSettingsIO::saveReadest(const ReadestCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadReadest(ReadestCredentialStore&, const char*, bool*);

 public:
  ReadestCredentialStore(const ReadestCredentialStore&) = delete;
  ReadestCredentialStore& operator=(const ReadestCredentialStore&) = delete;

  static ReadestCredentialStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  std::string getMd5Password() const;

  bool hasCredentials() const;
  void clearCredentials();

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  std::string getBaseUrl() const;
};

#define READEST_STORE ReadestCredentialStore::getInstance()
