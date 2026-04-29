#include "ReadestSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ctime>

#include "ReadestCredentialStore.h"

namespace {
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", READEST_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", READEST_STORE.getMd5Password().c_str());
  http.setAuthorization(READEST_STORE.getUsername().c_str(), READEST_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

using Error = ReadestSyncClient::Error;
}  // namespace

Error ReadestSyncClient::authenticate() {
  if (!READEST_STORE.hasCredentials()) {
    LOG_DBG("RDS", "No credentials configured");
    return Error::NO_CREDENTIALS;
  }

  const std::string baseUrl = READEST_STORE.getBaseUrl();
  if (baseUrl.empty()) {
    LOG_DBG("RDS", "No server URL configured");
    return Error::NO_CREDENTIALS;
  }

  const std::string url = baseUrl + "/users/auth";
  LOG_DBG("RDS", "Authenticating: %s", url.c_str());

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  http.end();

  LOG_DBG("RDS", "Auth response: %d", httpCode);

  if (httpCode == 200) return Error::OK;
  if (httpCode == 401) return Error::AUTH_FAILED;
  if (httpCode < 0) return Error::NETWORK_ERROR;
  return Error::SERVER_ERROR;
}

Error ReadestSyncClient::getProgress(const std::string& documentHash, KOReaderProgress& outProgress) {
  if (!READEST_STORE.hasCredentials()) {
    return Error::NO_CREDENTIALS;
  }

  const std::string baseUrl = READEST_STORE.getBaseUrl();
  if (baseUrl.empty()) {
    return Error::NO_CREDENTIALS;
  }

  const std::string url = baseUrl + "/syncs/progress/" + documentHash;
  LOG_DBG("RDS", "Getting progress: %s", url.c_str());

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, responseBody)) {
      return Error::JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("RDS", "Got progress: %.2f%%", outProgress.percentage * 100);
    return Error::OK;
  }

  http.end();

  if (httpCode == 401) return Error::AUTH_FAILED;
  if (httpCode == 404) return Error::NOT_FOUND;
  if (httpCode < 0) return Error::NETWORK_ERROR;
  return Error::SERVER_ERROR;
}

Error ReadestSyncClient::updateProgress(const KOReaderProgress& progress) {
  if (!READEST_STORE.hasCredentials()) {
    return Error::NO_CREDENTIALS;
  }

  const std::string baseUrl = READEST_STORE.getBaseUrl();
  if (baseUrl.empty()) {
    return Error::NO_CREDENTIALS;
  }

  const std::string url = baseUrl + "/syncs/progress";
  LOG_DBG("RDS", "Updating progress: %s", url.c_str());

  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  const int httpCode = http.PUT(body.c_str());
  http.end();

  LOG_DBG("RDS", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) return Error::OK;
  if (httpCode == 401) return Error::AUTH_FAILED;
  if (httpCode < 0) return Error::NETWORK_ERROR;
  return Error::SERVER_ERROR;
}
