#include "BookFusionTokenStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "../../src/JsonSettingsIO.h"

// Initialise static singleton instance
BookFusionTokenStore BookFusionTokenStore::instance;

namespace {
constexpr char BF_FILE_JSON[] = "/.crosspoint/bookfusion.json";
}  // namespace

bool BookFusionTokenStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveBookFusion(*this, BF_FILE_JSON);
}

bool BookFusionTokenStore::loadFromFile() {
  if (!Storage.exists(BF_FILE_JSON)) {
    LOG_DBG("BFS", "No BookFusion token file found");
    return false;
  }

  String json = Storage.readFile(BF_FILE_JSON);
  if (json.isEmpty()) {
    LOG_DBG("BFS", "BookFusion token file is empty");
    return false;
  }

  return JsonSettingsIO::loadBookFusion(*this, json.c_str());
}

void BookFusionTokenStore::setToken(const std::string& token) {
  accessToken = token;
  LOG_DBG("BFS", "BookFusion token set (%zu chars)", token.size());
}

void BookFusionTokenStore::clearToken() {
  accessToken.clear();
  saveToFile();
  LOG_DBG("BFS", "BookFusion token cleared");
}
