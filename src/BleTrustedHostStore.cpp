#include "BleTrustedHostStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>

BleTrustedHostStore BleTrustedHostStore::instance;

namespace {
constexpr char BLE_TRUSTED_HOSTS_FILE_JSON[] = "/.crosspoint/ble_trusted_hosts.json";
}

bool BleTrustedHostStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveBleTrustedHosts(*this, BLE_TRUSTED_HOSTS_FILE_JSON);
}

bool BleTrustedHostStore::loadFromFile() {
  if (!Storage.exists(BLE_TRUSTED_HOSTS_FILE_JSON)) return false;

  String json = Storage.readFile(BLE_TRUSTED_HOSTS_FILE_JSON);
  if (json.isEmpty()) return false;

  bool resave = false;
  const bool result = JsonSettingsIO::loadBleTrustedHosts(*this, json.c_str(), &resave);
  if (result && resave) {
    LOG_DBG("BTH", "Resaving BLE trusted hosts with obfuscated secrets");
    saveToFile();
  }
  return result;
}

bool BleTrustedHostStore::addOrReplaceHost(const BleTrustedHost& host) {
  if (host.hostId.empty() || host.secret.empty()) return false;

  const auto oldHosts = hosts;
  hosts.clear();
  hosts.push_back(host);
  if (!saveToFile()) {
    hosts = oldHosts;
    return false;
  }
  LOG_DBG("BTH", "Saved BLE trusted host: %s", host.hostId.c_str());
  return true;
}

bool BleTrustedHostStore::removeHost(const std::string& hostId) {
  const auto host =
      find_if(hosts.begin(), hosts.end(), [&hostId](const BleTrustedHost& item) { return item.hostId == hostId; });
  if (host == hosts.end()) return false;

  const auto oldHosts = hosts;
  hosts.erase(host);
  if (!saveToFile()) {
    hosts = oldHosts;
    return false;
  }
  LOG_DBG("BTH", "Removed BLE trusted host: %s", hostId.c_str());
  return true;
}

const BleTrustedHost* BleTrustedHostStore::findHost(const std::string& hostId) const {
  const auto host =
      find_if(hosts.begin(), hosts.end(), [&hostId](const BleTrustedHost& item) { return item.hostId == hostId; });
  return host == hosts.end() ? nullptr : &*host;
}

bool BleTrustedHostStore::clearAll() {
  const auto oldHosts = hosts;
  hosts.clear();
  if (!saveToFile()) {
    hosts = oldHosts;
    return false;
  }
  LOG_DBG("BTH", "Cleared BLE trusted hosts");
  return true;
}
