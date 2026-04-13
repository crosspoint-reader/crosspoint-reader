#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastEspError = 0;
unsigned KOReaderSyncClient::lastHeapAtFailure = 0;
unsigned KOReaderSyncClient::lastContigHeapAtFailure = 0;
const char* KOReaderSyncClient::lastOperation = "";

namespace {
bool g_keepSessionOpen = false;
esp_http_client_handle_t g_sessionClient = nullptr;

// Static buffer for the detail string returned by lastFailureDetail() — sized to fit
// the longest expected message including esp_err name (~32 chars), opcode (~10), heap
// numbers, and HTTP status. Single-threaded sync flow makes static safe.
char g_failureDetailBuf[160] = {0};
char g_lastResponsePreview[160] = {0};

std::string previewBody(const char* body, const size_t maxLen = 120) {
  if (!body || !*body) {
    return "<empty>";
  }

  std::string preview;
  preview.reserve(maxLen);
  for (const char* p = body; *p && preview.size() < maxLen; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '\r' || c == '\n' || c == '\t') {
      preview.push_back(' ');
    } else if (std::isprint(c)) {
      preview.push_back(static_cast<char>(c));
    } else {
      preview.push_back('?');
    }
  }

  if (strlen(body) > preview.size()) {
    preview += "...";
  }
  return preview;
}

void rememberResponsePreview(const char* body) {
  const std::string preview = previewBody(body, sizeof(g_lastResponsePreview) - 1);
  strncpy(g_lastResponsePreview, preview.c_str(), sizeof(g_lastResponsePreview) - 1);
  g_lastResponsePreview[sizeof(g_lastResponsePreview) - 1] = '\0';
}

// Reset the static diagnostic state at the start of each request and capture pre-flight
// heap so failure reporting always reflects what was available when the request started.
void beginRequest(const char* operation) {
  KOReaderSyncClient::lastOperation = operation;
  KOReaderSyncClient::lastEspError = 0;
  KOReaderSyncClient::lastHttpCode = 0;
  KOReaderSyncClient::lastHeapAtFailure = ESP.getFreeHeap();
  KOReaderSyncClient::lastContigHeapAtFailure = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  g_lastResponsePreview[0] = '\0';
}

// Skip a leading UTF-8 BOM (EF BB BF) and ASCII whitespace, returning a pointer
// to the first content character.  Used by response-body checks that verify the
// payload starts with '{' (JSON) rather than '<' (HTML captive-portal page).
const char* skipBomAndWhitespace(const char* p) {
  // UTF-8 BOM
  if (static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
      static_cast<unsigned char>(p[2]) == 0xBF) {
    p += 3;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  return p;
}

// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Use small HTTP/TLS buffers to reduce peak handshake memory on ESP32-C3.
// Payloads are tiny JSON, so throughput impact is minimal while avoiding
// large transient allocations from default client buffer sizes.
constexpr int HTTP_BUF_SIZE = 1024;
// Keep strict thresholding here. A small tolerance caused repeated handshake
// attempts in borderline-fragmented states that still failed in mbedTLS.
constexpr unsigned TLS_CONTIG_HEAP_TOLERANCE = 0;

// Captures radio/link state around failed connects.
// Why: many field failures look like TLS errors but are actually weak WiFi.
void logWifiSnapshot(const char* stage) {
  const wl_status_t status = WiFi.status();
  const int32_t rssi = WiFi.RSSI();
  LOG_DBG("KOSync", "%s: wifi_status=%d rssi=%ld ip=%s", stage, static_cast<int>(status), static_cast<long>(rssi),
          WiFi.localIP().toString().c_str());
}

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

ResponseBuffer g_sessionResponseBuf;

void clearResponseBuffer(ResponseBuffer* buf) {
  if (!buf) return;
  if (buf->data) {
    free(buf->data);
    buf->data = nullptr;
  }
  buf->len = 0;
  buf->capacity = 0;
}

void resetResponseBuffer(ResponseBuffer* buf) {
  if (!buf) return;
  buf->len = 0;
  if (buf->data) {
    buf->data[0] = '\0';
  }
}

ResponseBuffer* effectiveResponseBuffer(ResponseBuffer* localBuf) {
  return g_keepSessionOpen ? &g_sessionResponseBuf : localBuf;
}

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
  if (evt->event_id == HTTP_EVENT_REDIRECT && buf) {
    // A redirect is about to be followed. Clear any body already accumulated from
    // the redirect response (e.g. Werkzeug HTML page) so the final response body
    // accumulates cleanly without being prefixed by intermediate content.
    buf->len = 0;
    if (buf->data) buf->data[0] = '\0';
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
  const bool hasReusableSession = g_keepSessionOpen && g_sessionClient != nullptr;
  const bool isUpload =
      (KOReaderSyncClient::lastOperation && strcmp(KOReaderSyncClient::lastOperation, "update progress") == 0);
  const unsigned requiredContig = KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS;

  // Upload can often reuse the already-established GET connection. In that case
  // a full handshake allocation is typically unnecessary, so avoid failing fast
  // on contiguous-heap threshold and let the HTTP client attempt reuse.
  if (isUpload && hasReusableSession) {
    return true;
  }

  // beginRequest() already populated lastContigHeapAtFailure for the diagnostic path.
  if (KOReaderSyncClient::lastContigHeapAtFailure + TLS_CONTIG_HEAP_TOLERANCE < requiredContig) {
    LOG_ERR("KOSync", "Insufficient contiguous heap for TLS: %u available, %u required",
            KOReaderSyncClient::lastContigHeapAtFailure, requiredContig);
    // Synthesize an esp_err_t-shaped value so the diagnostic detail string is uniform.
    KOReaderSyncClient::lastEspError = ESP_ERR_NO_MEM;
    return false;
  }
  return true;
}

void refreshHeapSnapshot() {
  KOReaderSyncClient::lastHeapAtFailure = ESP.getFreeHeap();
  KOReaderSyncClient::lastContigHeapAtFailure = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
}

void logTlsAttemptPlan(const char* operation, int attempt) {
  const bool isUpload = (operation && strcmp(operation, "update progress") == 0);
  const bool hasReusableSession = g_keepSessionOpen && g_sessionClient != nullptr;
  const unsigned requiredContig = (isUpload && hasReusableSession) ? KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS_UPLOAD
                                                                   : KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS;

  LOG_DBG("KOSync", "%s attempt %d: keep_session=%s reusable_session=%s tls_mode=%s heap=%u contig=%u need=%u",
          operation ? operation : "request", attempt, g_keepSessionOpen ? "yes" : "no",
          hasReusableSession ? "yes" : "no", (isUpload && hasReusableSession) ? "reuse" : "handshake",
          KOReaderSyncClient::lastHeapAtFailure, KOReaderSyncClient::lastContigHeapAtFailure, requiredContig);
}

void resetSessionClientForRetry() {
  if (g_sessionClient) {
    esp_http_client_cleanup(g_sessionClient);
    g_sessionClient = nullptr;
  }
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  ResponseBuffer* activeBuf = effectiveResponseBuffer(buf);

  if (g_keepSessionOpen && g_sessionClient) {
    esp_http_client_set_url(g_sessionClient, url);
    esp_http_client_set_method(g_sessionClient, method);

    // KOSync auth headers
    esp_http_client_set_header(g_sessionClient, "Accept", "application/vnd.koreader.v1+json");
    esp_http_client_set_header(g_sessionClient, "x-auth-user", KOREADER_STORE.getUsername().c_str());
    esp_http_client_set_header(g_sessionClient, "x-auth-key", KOREADER_STORE.getMd5Password().c_str());

    std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
    std::string authHeader = "Basic " + base64Encode(credentials);
    esp_http_client_set_header(g_sessionClient, "Authorization", authHeader.c_str());
    return g_sessionClient;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = activeBuf;
  config.method = method;
  config.timeout_ms = 5000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = 512;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = g_keepSessionOpen;
  // Follow up to 3 redirects (e.g. HTTP→HTTPS, path normalization, DuckDNS proxy).
  // HTTP_EVENT_REDIRECT in httpEventHandler clears the buffer between hops so the
  // intermediate HTML bodies don't contaminate the final JSON response.
  config.max_redirection_count = 3;

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

  if (g_keepSessionOpen) {
    g_sessionClient = client;
  }

  return client;
}
}  // namespace

void KOReaderSyncClient::beginPersistentSession() {
  g_keepSessionOpen = true;
  clearResponseBuffer(&g_sessionResponseBuf);
}

void KOReaderSyncClient::endPersistentSession() {
  g_keepSessionOpen = false;
  if (g_sessionClient) {
    esp_http_client_cleanup(g_sessionClient);
    g_sessionClient = nullptr;
  }
  clearResponseBuffer(&g_sessionResponseBuf);
}

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
  ResponseBuffer* activeBuf = effectiveResponseBuffer(&buf);
  resetResponseBuffer(activeBuf);
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
  if (!g_keepSessionOpen) {
    esp_http_client_cleanup(client);
  }

  LOG_DBG("KOSync", "Register response: %d (err: %s) | body: %s", httpCode, esp_err_to_name(err),
          activeBuf->data ? activeBuf->data : "");

  if (err != ESP_OK) {
    return NETWORK_ERROR;
  }

  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;

  if (httpCode == 201) {
    return OK;
  } else if (httpCode == 200) {
    // Some server implementations return 200 when the user already exists
    return USER_EXISTS;
  } else if (httpCode == 402) {
    // Both "user already exists" (error 2002) and "registration disabled" (error 2005)
    // return HTTP 402 on the original kosync server. Distinguish them by body text.
    std::string lowerBody = activeBuf->data ? activeBuf->data : "";
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
  ResponseBuffer* activeBuf = effectiveResponseBuffer(&buf);
  resetResponseBuffer(activeBuf);
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) {
    lastEspError = ESP_ERR_NO_MEM;
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  lastEspError = err;
  if (!g_keepSessionOpen) {
    esp_http_client_cleanup(client);
  }

  LOG_DBG("KOSync", "Auth response: %d (err: %s)", httpCode, esp_err_to_name(err));

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;
  if (httpCode == 200) {
    // Guard against a reverse proxy or captive portal returning HTTP 200 + HTML.
    if (!activeBuf->data || *skipBomAndWhitespace(activeBuf->data) != '{') {
      return INVALID_RESPONSE;
    }
    return OK;
  }
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
  ResponseBuffer* activeBuf = effectiveResponseBuffer(&buf);
  esp_err_t err = ESP_FAIL;
  int httpCode = 0;

  for (int attempt = 1; attempt <= 2; attempt++) {
    // Retry attempts can happen after memory churn from a failed handshake.
    // Refresh heap snapshot each pass so preflight and diagnostics use current values.
    refreshHeapSnapshot();
    logTlsAttemptPlan("get progress", attempt);
    if (!checkHeapForTls()) {
      return NETWORK_ERROR;
    }

    resetResponseBuffer(activeBuf);

    esp_http_client_handle_t client = createClient(url.c_str(), &buf);
    if (!client) {
      lastEspError = ESP_ERR_NO_MEM;
      return NETWORK_ERROR;
    }

    logWifiSnapshot("WiFi before getProgress");
    err = esp_http_client_perform(client);
    httpCode = esp_http_client_get_status_code(client);
    lastHttpCode = httpCode;
    lastEspError = err;
    if (!g_keepSessionOpen) {
      esp_http_client_cleanup(client);
    }

    const size_t bodyLen = activeBuf->data ? strlen(activeBuf->data) : 0;
    LOG_DBG("KOSync", "GET %s -> %d (err: %s) [attempt %d body_len=%u]", url.c_str(), httpCode, esp_err_to_name(err),
            attempt, static_cast<unsigned>(bodyLen));
    if (err == ESP_OK && (httpCode < 200 || httpCode >= 300)) {
      rememberResponsePreview(activeBuf->data);
      LOG_ERR("KOSync", "GET failure body preview: %s", g_lastResponsePreview);
    }

    // Retry exactly once for connect-level failures only.
    // Why: this recovers short AP/roaming hiccups without masking persistent
    // TLS/auth/server errors that should be surfaced immediately.
    if (err == ESP_OK || err != ESP_ERR_HTTP_CONNECT || attempt == 2) {
      break;
    }

    // Failed connect can leave a persistent client handle in a bad state.
    // Recreate it before retry so we don't repeat work on a stale transport.
    resetSessionClientForRetry();

    LOG_ERR("KOSync", "getProgress connect failed on attempt %d, retrying once", attempt);
    logWifiSnapshot("WiFi before getProgress retry");
    delay(400);
  }

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;

  if (httpCode == 200 && activeBuf->data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, activeBuf->data);

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    // kosync convention: no stored progress for the document is signalled by
    // HTTP 200 with an empty body ("{}"), not by 404. Detect that here so the
    // caller doesn't apply a zeroed-out position as if it were real progress.
    if (doc["document"].isNull() || doc["progress"].isNull()) {
      LOG_DBG("KOSync", "Empty progress payload — treating as not found");
      return NOT_FOUND;
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
  if (httpCode == 404) {
    LOG_DBG("KOSync", "GET progress returned 404 for %s - treating as NOT_FOUND", url.c_str());
    return NOT_FOUND;
  }
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
  ResponseBuffer* activeBuf = effectiveResponseBuffer(&buf);
  esp_err_t err = ESP_FAIL;
  int httpCode = 0;

  for (int attempt = 1; attempt <= 2; attempt++) {
    // Retry attempts can happen after memory churn from a failed handshake.
    // Refresh heap snapshot each pass so preflight and diagnostics use current values.
    refreshHeapSnapshot();
    logTlsAttemptPlan("update progress", attempt);
    if (!checkHeapForTls()) {
      return NETWORK_ERROR;
    }

    resetResponseBuffer(activeBuf);

    esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
    if (!client) {
      lastEspError = ESP_ERR_NO_MEM;
      return NETWORK_ERROR;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), body.length());

    logWifiSnapshot("WiFi before updateProgress");
    err = esp_http_client_perform(client);
    httpCode = esp_http_client_get_status_code(client);
    lastHttpCode = httpCode;
    lastEspError = err;
    if (!g_keepSessionOpen) {
      esp_http_client_cleanup(client);
    }

    const size_t bodyLen = activeBuf->data ? strlen(activeBuf->data) : 0;
    LOG_DBG("KOSync", "PUT %s -> %d (err: %s) [attempt %d body_len=%u]", url.c_str(), httpCode, esp_err_to_name(err),
            attempt, static_cast<unsigned>(bodyLen));
    if (err == ESP_OK && (httpCode < 200 || httpCode >= 300)) {
      rememberResponsePreview(activeBuf->data);
      LOG_ERR("KOSync", "PUT failure body preview: %s", g_lastResponsePreview);
      LOG_ERR("KOSync", "PUT failure request summary: document=%s percentage=%.4f progress=%s",
              progress.document.c_str(), progress.percentage, progress.progress.c_str());
    }

    // Retry exactly once for connect-level failures only.
    // Why: same policy as GET keeps behavior predictable across both endpoints.
    if (err == ESP_OK || err != ESP_ERR_HTTP_CONNECT || attempt == 2) {
      break;
    }

    // Failed connect can leave a persistent client handle in a bad state.
    // Recreate it before retry so we don't repeat work on a stale transport.
    resetSessionClientForRetry();

    LOG_ERR("KOSync", "updateProgress connect failed on attempt %d, retrying once", attempt);
    logWifiSnapshot("WiFi before updateProgress retry");
    delay(400);
  }

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;
  if (httpCode == 200 || httpCode == 202) {
    // Guard against a reverse proxy or captive portal returning HTTP 200 + HTML.
    if (activeBuf->data) {
      const char c = *skipBomAndWhitespace(activeBuf->data);
      if (c != '\0' && c != '{') {
        return INVALID_RESPONSE;
      }
    }
    return OK;
  }
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::lastFailureDetail() {
  const bool isUpload = (lastOperation && strcmp(lastOperation, "update progress") == 0);
  const bool hasReusableSession = g_keepSessionOpen && g_sessionClient != nullptr;
  const unsigned requiredContig =
      (isUpload && hasReusableSession) ? MIN_CONTIG_HEAP_FOR_TLS_UPLOAD : MIN_CONTIG_HEAP_FOR_TLS;

  // Heap-pressure case: surfaced when checkHeapForTls() refused before any TCP/TLS work happened.
  if (lastEspError == ESP_ERR_NO_MEM && lastHttpCode == 0) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
             "%s: low memory (%u free, %u contig, need %u). Reboot device.", lastOperation, lastHeapAtFailure,
             lastContigHeapAtFailure, requiredContig);
    return g_failureDetailBuf;
  }
  // Network/TLS case: esp_http_client_perform() failed before getting a status code.
  if (lastHttpCode == 0 && lastEspError != 0) {
    // HTTPS connect failures can be TLS version issues (ESP32 only supports up
    // to TLS 1.2), but also DNS, cert, or plain network problems. Include the
    // error name and heap stats so the user/bug-report has enough to triage.
    if (lastEspError == ESP_ERR_HTTP_CONNECT && KOREADER_STORE.getBaseUrl().rfind("https", 0) == 0) {
      snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
               "%s: connect failed — check network, DNS, certs, or TLS 1.2 compat (heap %u/%u contig)", lastOperation,
               lastHeapAtFailure, lastContigHeapAtFailure);
    } else {
      snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf), "%s: %s (heap %u/%u contig)", lastOperation,
               esp_err_to_name(lastEspError), lastHeapAtFailure, lastContigHeapAtFailure);
    }
    return g_failureDetailBuf;
  }
  // Invalid-response case: HTTP 200/202 but body was not JSON (e.g. captive portal HTML).
  // On real success callers never reach lastFailureDetail(), so a 2xx here means INVALID_RESPONSE.
  if ((lastHttpCode == 200 || lastHttpCode == 202) && lastEspError == ESP_OK) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
             "%s: expected JSON but received HTML (captive portal or proxy?)", lastOperation);
    return g_failureDetailBuf;
  }
  // Server case: got an HTTP status the client didn't recognize as success.
  if (lastHttpCode != 0) {
    if (lastHttpCode == 404 && lastOperation && strcmp(lastOperation, "update progress") == 0) {
      std::string lowerPreview = g_lastResponsePreview;
      std::transform(lowerPreview.begin(), lowerPreview.end(), lowerPreview.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lowerPreview.find("book not found") != std::string::npos) {
        snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
                 "%s: server does not know this book yet; server expects the same file to already exist there, usually "
                 "downloaded via OPDS",
                 lastOperation);
        return g_failureDetailBuf;
      }
      snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
               "%s: HTTP 404 (upload rejected; server may require book to be known)", lastOperation);
      return g_failureDetailBuf;
    }
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
    case REDIRECT_ERROR:
      return "Server redirected (check server URL)";
    case INVALID_RESPONSE:
      return "Unexpected response (check server URL)";
    default:
      return "Unknown error";
  }
}
