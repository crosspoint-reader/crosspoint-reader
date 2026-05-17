#pragma once

#include <string>
#include <vector>

struct BleTrustedHost {
  std::string hostId;
  std::string name;
  std::string secret;  // Plaintext in memory; obfuscated with hardware key on disk
};

class BleTrustedHostStore;
namespace JsonSettingsIO {
bool saveBleTrustedHosts(const BleTrustedHostStore& store, const char* path);
bool loadBleTrustedHosts(BleTrustedHostStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

class BleTrustedHostStore {
 private:
  static BleTrustedHostStore instance;
  std::vector<BleTrustedHost> hosts;

  static constexpr size_t MAX_HOSTS = 1;

  friend bool JsonSettingsIO::saveBleTrustedHosts(const BleTrustedHostStore&, const char*);
  friend bool JsonSettingsIO::loadBleTrustedHosts(BleTrustedHostStore&, const char*, bool*);

 public:
  BleTrustedHostStore(const BleTrustedHostStore&) = delete;
  BleTrustedHostStore& operator=(const BleTrustedHostStore&) = delete;

  static BleTrustedHostStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addOrReplaceHost(const BleTrustedHost& host);
  bool removeHost(const std::string& hostId);
  const BleTrustedHost* findHost(const std::string& hostId) const;
  bool hasHosts() const { return !hosts.empty(); }
  bool clearAll();

  const std::vector<BleTrustedHost>& getHosts() const { return hosts; }

 private:
  BleTrustedHostStore() = default;
};

#define BLE_TRUSTED_HOSTS BleTrustedHostStore::getInstance()
