#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include <string>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// wolfSSL uses the default allocator, which can use PSRAM on supported builds.
// Keep a free-space floor and room for a full TLS record when the server does
// not negotiate our smaller record limit. These are preflight margins, not a
// guarantee that a handshake will fit.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// Apply the shared KOSync auth headers after begin(). x-auth-* is the native
// KOSync scheme; Basic auth is added for Calibre-Web-Automated compatibility.
void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < MIN_FREE_FOR_TLS || heap.largestBlockBytes < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync",
            "Insufficient allocatable heap for TLS handshake: %zu bytes free (need %u), %zu max alloc (need %u)",
            heap.freeBytes, MIN_FREE_FOR_TLS, heap.largestBlockBytes, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  // Any 2xx is success. The reference kosync server answers 200, but
  // KOSync-compatible implementations differ (BookLore/grimmory is a Spring
  // service and uses the idiomatic codes) — see issue #2876.
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Create user response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;  // 2xx: created (see #2876)
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }

  // 204 = success with no stored progress for this document (Spring-style
  // KOSync implementations; the reference server answers 200 with an empty
  // object instead). Map it to the same graceful no-remote-progress path as
  // 404 rather than falling through to SERVER_ERROR — see issue #2876.
  if (httpCode == 204) {
    http.end();
    return NOT_FOUND;
  }

  if (httpCode >= 200 && httpCode < 300) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, http.getString().c_str());
    http.end();

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

    outProgress.position.reset();
    if (KOREADER_STORE.usesCrossPointSyncServer()) {
      const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
      if (!pos.isNull()) {
        KOReaderRichPosition rich;
        rich.pctQ = pos["pctQ"].as<uint32_t>();
        rich.spineIndex = pos["spine"].as<uint16_t>();
        rich.pageNumber = pos["page"].as<uint16_t>();
        const uint16_t pages = pos["pages"].as<uint16_t>();
        rich.totalPages = pages > 0 ? pages : 1;
        const uint16_t para = pos["para"].as<uint16_t>();
        if (para > 0) rich.paragraphIndex = para;
        rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
        LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                rich.totalPages, para);
        outProgress.position = std::move(rich);
      }
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
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

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;
  if (progress.position.has_value() && KOREADER_STORE.usesCrossPointSyncServer()) {
    // CrossPoint-specific extension: do not send it to third-party KOSync servers.
    const auto& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PUT", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  // Any 2xx accepts the progress. The reference kosync server answers 200,
  // but Spring-based KOSync implementations (BookLore/grimmory) answer a PUT
  // with the idiomatic 201/204, which used to land in SERVER_ERROR and made
  // every sync against them fail after a successful pull — issue #2876.
  if (httpCode >= 200 && httpCode < 300) return OK;
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
