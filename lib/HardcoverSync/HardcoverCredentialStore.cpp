#include "HardcoverCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "../../src/JsonSettingsIO.h"

// Initialize the static instance
HardcoverCredentialStore HardcoverCredentialStore::instance;

namespace {
constexpr char HARDCOVER_FILE_JSON[] = "/.crosspoint/hardcover.json";
constexpr size_t MAX_HARDCOVER_FILE_BYTES = 4096;  // A valid credentials JSON is ~200 bytes; 4 KB provides
                                                    // ample margin while preventing heap exhaustion from
                                                    // corrupt or attacker-controlled files.
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

  // Guard against oversized / corrupt files consuming all heap
  HalFile file;
  if (!Storage.openFileForRead("HCS", HARDCOVER_FILE_JSON, file)) {
    LOG_ERR("HCS", "Failed to open credentials file");
    return false;
  }
  const size_t fileBytes = file.size();
  file.close();
  if (fileBytes > MAX_HARDCOVER_FILE_BYTES) {
    LOG_ERR("HCS", "Credentials file too large (%zu bytes), ignoring", fileBytes);
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
  const std::string backup = token;
  token.clear();
  if (!saveToFile()) {
    // Persist failed: restore the in-memory token so disk and RAM stay in sync
    token = backup;
    LOG_ERR("HCS", "Failed to persist cleared Hardcover token — keeping existing token");
    return;
  }
  LOG_DBG("HCS", "Cleared Hardcover token");
}
