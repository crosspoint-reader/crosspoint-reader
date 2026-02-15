#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  // HTTP Basic Auth (RFC 7617) header. This is needed to support koreader sync server embedded in Calibre Web Automated
  // (https://github.com/crocodilestick/Calibre-Web-Automated/blob/main/cps/progress_syncing/protocols/kosync.py)
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s", url.c_str());

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

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode == 200) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s", url.c_str());

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
    // Parse JSON response from response string
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode == 404) {
    return NOT_FOUND;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s", url.c_str());

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

  // Build JSON body (timestamp not required per API spec)
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  const int httpCode = http.PUT(body.c_str());
  http.end();

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    default:
      return "Unknown error";
  }
}
