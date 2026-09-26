#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <array>
#include <string>
#include <vector>

#include "network/HttpDownloader.h"

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk

  // Extra headers sent with every request to this server (e.g. a Cloudflare
  // Access service token). Fixed-size to avoid vector growth on the settings
  // edit path; an empty name marks an unused slot.
  static constexpr size_t MAX_CUSTOM_HEADERS = 2;
  std::array<HttpHeader, MAX_CUSTOM_HEADERS> customHeaders;

  // Headers with a non-empty name, ready to hand to HttpDownloader.
  std::vector<HttpHeader> activeCustomHeaders() const;
};

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore : public PersistableStore<OpdsServerStore> {
 private:
  std::vector<OpdsServer> servers;

  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore() = default;

  friend class PersistableStore<OpdsServerStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/opds.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const { return servers; }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const { return servers.size(); }
  bool hasServers() const { return !servers.empty(); }
};

#define OPDS_STORE OpdsServerStore::getInstance()
