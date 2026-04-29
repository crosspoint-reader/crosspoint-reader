#include "ReadestCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include "../../src/JsonSettingsIO.h"

ReadestCredentialStore ReadestCredentialStore::instance;

namespace {
constexpr char READEST_FILE_JSON[] = "/.crosspoint/readest.json";
}

bool ReadestCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadest(*this, READEST_FILE_JSON);
}

bool ReadestCredentialStore::loadFromFile() {
  const String json = Storage.readFile(READEST_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }
  bool resave = false;
  const bool result = JsonSettingsIO::loadReadest(*this, json.c_str(), &resave);
  if (result && resave) {
    saveToFile();
    LOG_DBG("RDS", "Resaved Readest credentials to update format");
  }
  return result;
}

void ReadestCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("RDS", "Set credentials for user: %s", user.c_str());
}

std::string ReadestCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return std::string(md5.toString().c_str());
}

bool ReadestCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void ReadestCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("RDS", "Cleared Readest credentials");
}

void ReadestCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("RDS", "Set server URL: %s", url.empty() ? "(none)" : url.c_str());
}

std::string ReadestCredentialStore::getBaseUrl() const {
  if (serverUrl.empty()) {
    return "";
  }

  if (serverUrl.find("://") == std::string::npos) {
    return "http://" + serverUrl;
  }

  return serverUrl;
}
