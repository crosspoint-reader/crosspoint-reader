#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFiClientSecure.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Small TLS buffers to fit in ESP32-C3's limited heap (~46KB free after WiFi).
// KOSync payloads are tiny JSON (<1KB), so 2KB buffers are sufficient.
// Default 16KB buffers cause OOM during TLS handshake.
constexpr int HTTP_BUF_SIZE = 2048;

// Cloudflare tunnels send a 3-cert Google Trust Services chain. During the TLS handshake
// mbedTLS makes many small allocations that collectively consume ~48KB of heap. With only
// ~50KB free after WiFi connects, the session drove min-free-ever down to 2600 bytes before
// failing with MBEDTLS_ERR_X509_ALLOC_FAILED (-0x2880). Check total free heap (not max
// contiguous block) because the failure mode is aggregate exhaustion, not one large alloc.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Response buffer for reading HTTP body
struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    char* newData = (char*)realloc(data, size);
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

// HTTP event handler to collect response body
esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("KOSync", "Response buffer allocation failed (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = 15000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  // Verified-TLS path only. The "accept untrusted cert" path never reaches here —
  // it uses performInsecure() (WiFiClientSecure), because the prebuilt Arduino SDK
  // (CONFIG_ESP_TLS_INSECURE unset) gives esp_http_client no way to skip verification.
  config.crt_bundle_attach = esp_crt_bundle_attach;

  // HTTP Basic Auth for Calibre-Web-Automated compatibility
  config.username = KOREADER_STORE.getUsername().c_str();
  config.password = KOREADER_STORE.getPassword().c_str();
  config.auth_type = HTTP_AUTH_TYPE_BASIC;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  // KOSync auth headers
  if (esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json") != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-user", KOREADER_STORE.getUsername().c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-key", KOREADER_STORE.getMd5Password().c_str()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set auth headers");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  return client;
}

// Insecure HTTPS request for self-signed / untrusted servers.
// esp_http_client cannot skip cert verification on the prebuilt Arduino SDK
// (CONFIG_ESP_TLS_INSECURE is not set), so this path uses WiFiClientSecure,
// which drives mbedTLS directly and honors setInsecure(). Returns the HTTP
// status code (>0), or a negative HTTPClient error on connection failure.
// On success the response body is written to outResponse.
int performInsecure(const char* url, esp_http_client_method_t method, const std::string& body,
                    std::string& outResponse) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();  // accept any server certificate

  HTTPClient http;
  if (!http.begin(secureClient, url)) {
    LOG_ERR("KOSync", "Insecure HTTPClient begin failed");
    return -1;
  }
  http.setTimeout(15000);
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  int code;
  if (method == HTTP_METHOD_PUT) {
    http.addHeader("Content-Type", "application/json");
    code = http.PUT(reinterpret_cast<uint8_t*>(const_cast<char*>(body.data())), body.size());
  } else {
    code = http.GET();
  }

  if (code > 0) outResponse = http.getString().c_str();
  http.end();
  return code;
}

// Parse a /syncs/progress JSON response body into outProgress.
KOReaderSyncClient::Error parseProgress(const char* body, const std::string& documentHash,
                                        KOReaderProgress& outProgress) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body);
  if (error) {
    LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
    return KOReaderSyncClient::JSON_ERROR;
  }

  outProgress.document = documentHash;
  outProgress.progress = doc["progress"].as<std::string>();
  outProgress.percentage = doc["percentage"].as<float>();
  outProgress.device = doc["device"].as<std::string>();
  outProgress.deviceId = doc["device_id"].as<std::string>();
  outProgress.timestamp = doc["timestamp"].as<int64_t>();

  LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
  return KOReaderSyncClient::OK;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  if (KOREADER_STORE.getAllowUntrustedCert()) {
    std::string resp;
    const int httpCode = performInsecure(url.c_str(), HTTP_METHOD_GET, "", resp);
    lastHttpCode = httpCode;
    LOG_DBG("KOSync", "Auth response (insecure): %d", httpCode);
    if (httpCode <= 0) return NETWORK_ERROR;
    if (httpCode == 200) return OK;
    if (httpCode == 401) return AUTH_FAILED;
    return SERVER_ERROR;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  if (KOREADER_STORE.getAllowUntrustedCert()) {
    std::string resp;
    const int httpCode = performInsecure(url.c_str(), HTTP_METHOD_GET, "", resp);
    lastHttpCode = httpCode;
    LOG_DBG("KOSync", "Get progress response (insecure): %d", httpCode);
    if (httpCode <= 0) return NETWORK_ERROR;
    if (httpCode == 200) return parseProgress(resp.c_str(), documentHash, outProgress);
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode == 404) return NOT_FOUND;
    return SERVER_ERROR;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Get progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 && buf.data) return parseProgress(buf.data, documentHash, outProgress);
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  if (KOREADER_STORE.getAllowUntrustedCert()) {
    std::string resp;
    const int httpCode = performInsecure(url.c_str(), HTTP_METHOD_PUT, body, resp);
    lastHttpCode = httpCode;
    LOG_DBG("KOSync", "Update progress response (insecure): %d", httpCode);
    if (httpCode <= 0) return NETWORK_ERROR;
    if (httpCode == 200 || httpCode == 202) return OK;
    if (httpCode == 401) return AUTH_FAILED;
    return SERVER_ERROR;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request body");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Update progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
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
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    default:
      return "Unknown error";
  }
}
