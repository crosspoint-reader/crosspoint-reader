#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastEspError = 0;
unsigned KOReaderSyncClient::lastHeapAtFailure = 0;
unsigned KOReaderSyncClient::lastContigHeapAtFailure = 0;
const char* KOReaderSyncClient::lastOperation = "";

namespace {
// Static buffer for the detail string returned by lastFailureDetail() — sized to fit
// the longest expected message including esp_err name (~32 chars), opcode (~10), heap
// numbers, and HTTP status. Single-threaded sync flow makes static safe.
char g_failureDetailBuf[160] = {0};

// Reset the static diagnostic state at the start of each request and capture pre-flight
// heap so failure reporting always reflects what was available when the request started.
void beginRequest(const char* operation) {
  KOReaderSyncClient::lastOperation = operation;
  KOReaderSyncClient::lastEspError = 0;
  KOReaderSyncClient::lastHttpCode = 0;
  KOReaderSyncClient::lastHeapAtFailure = ESP.getFreeHeap();
  KOReaderSyncClient::lastContigHeapAtFailure = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
}

// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Small TLS buffers to fit in ESP32-C3's limited heap (~46KB free after WiFi).
// KOSync payloads are tiny JSON (<1KB), so 2KB buffers are sufficient.
// Default 16KB buffers cause OOM during TLS handshake.
constexpr int HTTP_BUF_SIZE = 2048;

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

// Base64 encode for HTTP Basic Auth
std::string base64Encode(const std::string& input) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  int val = 0, valb = -6;
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

// Verify there is enough contiguous heap to attempt a TLS handshake. mbedTLS needs a
// large contiguous block during the handshake (~24-32 KB depending on cert chain depth).
// Total free heap can mislead because fragmentation leaves no single block big enough,
// which is precisely the scenario after recent PNG/JPG decode activity. Returns true if
// we should proceed; false means caller must abort with NETWORK_ERROR — in which case
// lastFailureDetail() will report the heap shortage instead of attempting a doomed handshake.
bool checkHeapForTls() {
  // beginRequest() already populated lastContigHeapAtFailure for the diagnostic path.
  if (KOReaderSyncClient::lastContigHeapAtFailure < KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient contiguous heap for TLS: %u available, %u required",
            KOReaderSyncClient::lastContigHeapAtFailure, KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS);
    // Synthesize an esp_err_t-shaped value so the diagnostic detail string is uniform.
    KOReaderSyncClient::lastEspError = ESP_ERR_NO_MEM;
    return false;
  }
  return true;
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
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  // KOSync auth headers
  esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json");
  esp_http_client_set_header(client, "x-auth-user", KOREADER_STORE.getUsername().c_str());
  esp_http_client_set_header(client, "x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  // HTTP Basic Auth for Calibre-Web-Automated compatibility
  std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  std::string authHeader = "Basic " + base64Encode(credentials);
  esp_http_client_set_header(client, "Authorization", authHeader.c_str());

  return client;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::registerUser() {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  beginRequest("register");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Registering user: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Register request body: <redacted credentials>");

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) {
    lastEspError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), body.length());

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastEspError = err;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Register response: %d (err: %s) | body: %s", httpCode, esp_err_to_name(err),
          buf.data ? buf.data : "");

  if (err != ESP_OK) {
    return NETWORK_ERROR;
  }

  if (httpCode == 201) {
    return OK;
  } else if (httpCode == 200) {
    // Some server implementations return 200 when the user already exists
    return USER_EXISTS;
  } else if (httpCode == 402) {
    // Both "user already exists" (error 2002) and "registration disabled" (error 2005)
    // return HTTP 402 on the original kosync server. Distinguish them by body text.
    std::string lowerBody = buf.data ? buf.data : "";
    std::transform(lowerBody.begin(), lowerBody.end(), lowerBody.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerBody.find("already") != std::string::npos) {
      return USER_EXISTS;
    }
    return REGISTRATION_DISABLED;
  } else if (httpCode == 409) {
    // korrosync returns 409 for existing users
    return USER_EXISTS;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  beginRequest("auth");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastEspError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastEspError = err;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Auth response: %d (err: %s)", httpCode, esp_err_to_name(err));

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  beginRequest("get progress");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastEspError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastEspError = err;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Get progress response: %d (err: %s)", httpCode, esp_err_to_name(err));

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);

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

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  beginRequest("update progress");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

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

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) {
    lastEspError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), body.length());

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastEspError = err;
  esp_http_client_cleanup(client);

  LOG_DBG("KOSync", "Update progress response: %d (err: %s)", httpCode, esp_err_to_name(err));

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::lastFailureDetail() {
  // Heap-pressure case: surfaced when checkHeapForTls() refused before any TCP/TLS work happened.
  if (lastEspError == ESP_ERR_NO_MEM && lastHttpCode == 0) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
             "%s: low memory (%u free, %u contig, need %u). Reboot device.", lastOperation, lastHeapAtFailure,
             lastContigHeapAtFailure, MIN_CONTIG_HEAP_FOR_TLS);
    return g_failureDetailBuf;
  }
  // Network/TLS case: esp_http_client_perform() failed before getting a status code.
  if (lastHttpCode == 0 && lastEspError != 0) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf), "%s: %s (heap %u/%u contig)", lastOperation,
             esp_err_to_name(lastEspError), lastHeapAtFailure, lastContigHeapAtFailure);
    return g_failureDetailBuf;
  }
  // Server case: got an HTTP status the client didn't recognize as success.
  if (lastHttpCode != 0) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf), "%s: HTTP %d", lastOperation, lastHttpCode);
    return g_failureDetailBuf;
  }
  // No prior request, or success.
  g_failureDetailBuf[0] = '\0';
  return g_failureDetailBuf;
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
    case USER_EXISTS:
      return "Username is already taken";
    case REGISTRATION_DISABLED:
      return "Registration is disabled on this server";
    default:
      return "Unknown error";
  }
}
