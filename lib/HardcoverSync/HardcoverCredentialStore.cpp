#include "HardcoverCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "../../src/JsonSettingsIO.h"

// Initialize the static instance
HardcoverCredentialStore HardcoverCredentialStore::instance;

namespace {
constexpr char HARDCOVER_FILE_JSON[] = "/.crosspoint/hardcover.json";
}  // namespace

bool HardcoverCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveHardcover(*this, HARDCOVER_FILE_JSON);
}

bool HardcoverCredentialStore::loadFromFile() {
  if (!Storage.exists(HARDCOVER_FILE_JSON)) {
    LOG_DBG("HCS", "No credentials file found");
    return false;
  }

  String json = Storage.readFile(HARDCOVER_FILE_JSON);
  if (json.isEmpty()) {
    LOG_ERR("HCS", "Empty credentials file");
    return false;
  }

  return JsonSettingsIO::loadHardcover(*this, json.c_str());
}

void HardcoverCredentialStore::setToken(const std::string& bearerToken) {
  token = bearerToken;
  LOG_DBG("HCS", "Token set (length=%zu)", bearerToken.size());
}

bool HardcoverCredentialStore::hasToken() const { return !token.empty(); }

void HardcoverCredentialStore::clearToken() {
  token.clear();
  saveToFile();
  LOG_DBG("HCS", "Cleared Hardcover token");
}
