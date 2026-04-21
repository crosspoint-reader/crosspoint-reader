#include "BookFusionSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <cstring>

#include "BookFusionTokenStore.h"

namespace {
// Add auth and accept headers to an authenticated request.
void addAuthHeaders(HTTPClient& http) {
  const std::string bearer = "Bearer " + BF_TOKEN_STORE.getToken();
  http.addHeader("Authorization", bearer.c_str());
  http.addHeader("Accept", BookFusionSyncClient::API_ACCEPT);
}
}  // namespace

// --- Device Code Auth ---

BookFusionSyncClient::Error BookFusionSyncClient::requestDeviceCode(BookFusionDeviceCodeResponse& out) {
  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/auth/device", BASE_URL);
  LOG_DBG("BFS", "Requesting device code: %s", url);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["client_id"] = CLIENT_ID;
  String bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  String responseBody = http.getString();
  http.end();

  LOG_DBG("BFS", "requestDeviceCode response: %d", httpCode);

  if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  if (httpCode != 200) {
    return SERVER_ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
    LOG_ERR("BFS", "requestDeviceCode JSON parse error");
    return JSON_ERROR;
  }

  strlcpy(out.deviceCode, doc["device_code"] | "", sizeof(out.deviceCode));
  strlcpy(out.userCode, doc["user_code"] | "", sizeof(out.userCode));
  strlcpy(out.verificationUri, doc["verification_uri"] | "", sizeof(out.verificationUri));
  out.interval = doc["interval"] | 5;
  out.expiresIn = doc["expires_in"] | 600;

  LOG_DBG("BFS", "Device code received: user_code=%s, interval=%ds, expires_in=%ds", out.userCode, out.interval,
          out.expiresIn);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::pollForToken(const char* deviceCode, char* outToken,
                                                               size_t tokenMaxLen) {
  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/auth/token", BASE_URL);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["grant_type"] = DEVICE_CODE_GRANT_TYPE;
  body["client_id"] = CLIENT_ID;
  body["device_code"] = deviceCode;
  String bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  String responseBody = http.getString();
  http.end();

  LOG_DBG("BFS", "pollForToken response: %d", httpCode);

  if (httpCode < 0) {
    return NETWORK_ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
    LOG_ERR("BFS", "pollForToken JSON parse error");
    return JSON_ERROR;
  }

  if (httpCode == 200) {
    const char* token = doc["access_token"] | "";
    if (token[0] == '\0') {
      return JSON_ERROR;
    }
    strlcpy(outToken, token, tokenMaxLen);
    LOG_DBG("BFS", "Token received");
    return OK;
  }

  // Map OAuth error codes
  const char* errCode = doc["error"] | "";
  LOG_DBG("BFS", "pollForToken error: %s", errCode);

  if (strcmp(errCode, "authorization_pending") == 0) return PENDING;
  if (strcmp(errCode, "slow_down") == 0) return SLOW_DOWN;
  if (strcmp(errCode, "expired_token") == 0) return EXPIRED;
  if (strcmp(errCode, "access_denied") == 0) return DENIED;
  // BookFusion returns "invalid_grant" (HTTP 400) while authorization is still
  // pending — non-standard, but the official Lua plugin keeps polling on any
  // unrecognised error, so we do the same.
  if (strcmp(errCode, "invalid_grant") == 0) return PENDING;

  return SERVER_ERROR;
}

// --- Progress ---

BookFusionSyncClient::Error BookFusionSyncClient::getProgress(uint32_t bookId, BookFusionPosition& out) {
  if (!BF_TOKEN_STORE.hasToken()) {
    return NO_TOKEN;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  LOG_DBG("BFS", "getProgress: %s", url);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);

  const int httpCode = http.GET();

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
      LOG_ERR("BFS", "getProgress JSON parse error");
      return JSON_ERROR;
    }

    out.percentage = doc["percentage"] | 0.0f;
    out.chapterIndex = doc["chapter_index"] | 0;
    out.pagePositionInBook = doc["page_position_in_book"] | 0.0f;

    LOG_DBG("BFS", "Remote progress: %.2f%%, chapter %d", out.percentage, out.chapterIndex);
    return OK;
  }

  http.end();
  LOG_DBG("BFS", "getProgress response: %d", httpCode);

  if (httpCode == 404) return NOT_FOUND;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

BookFusionSyncClient::Error BookFusionSyncClient::setProgress(uint32_t bookId, const BookFusionPosition& pos) {
  if (!BF_TOKEN_STORE.hasToken()) {
    return NO_TOKEN;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  LOG_DBG("BFS", "setProgress: %s (%.2f%%)", url, pos.percentage);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["percentage"] = pos.percentage;
  body["chapter_index"] = pos.chapterIndex;
  body["page_position_in_book"] = pos.pagePositionInBook;
  String bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  http.end();

  LOG_DBG("BFS", "setProgress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 201) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

// --- Library Browse & Download ---

BookFusionSyncClient::Error BookFusionSyncClient::searchBooks(int page, BookFusionSearchResult& out, const char* list,
                                                              const char* sort) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/search", BASE_URL);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  // 8 books per display page keeps the raw response under ~20 KB.
  // Arduino String grows by doubling: a 53 KB response (21 books) needs a
  // ~64 KB buffer during the final realloc, pushing peak heap above 113 KB.
  // With 8 books the response is ~20 KB → peak ~40 KB, well within budget.
  // Request 9 to detect hasMore without needing response headers.
  static constexpr int BOOKS_PER_PAGE = 8;

  JsonDocument reqBody;
  reqBody["page"] = page;
  reqBody["per_page"] = BOOKS_PER_PAGE + 1;
  reqBody["sort"] = (sort != nullptr) ? sort : "added_at-desc";
  if (list != nullptr) {
    reqBody["list"] = list;
  }
  String bodyStr;
  serializeJson(reqBody, bodyStr);

  const int httpCode = http.POST(bodyStr);
  LOG_DBG("BFS", "searchBooks page=%d response: %d", page, httpCode);

  if (httpCode < 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (httpCode == 401) {
    http.end();
    return AUTH_FAILED;
  }
  if (httpCode != 200) {
    http.end();
    return SERVER_ERROR;
  }

  // Read the full response body before parsing. Streaming from WiFiClientSecure
  // causes IncompleteInput errors because TLS chunks arrive after ArduinoJson
  // has already read past the end of what was buffered.
  String responseBody = http.getString();
  http.end();

  // Build a filter that discards every field except the four we need.
  // BookFusion books carry ~20 fields (cover URLs, descriptions, etc.); keeping
  // only what we display reduces JsonDocument heap from ~30 KB to ~5 KB.
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["format"] = true;
  filter[0]["authors"][0]["name"] = true;

  JsonDocument doc;
  const auto parseErr = deserializeJson(doc, responseBody, DeserializationOption::Filter(filter));

  if (parseErr != DeserializationError::Ok) {
    LOG_ERR("BFS", "searchBooks JSON parse error: %s", parseErr.c_str());
    return JSON_ERROR;
  }

  if (!doc.is<JsonArray>()) {
    LOG_ERR("BFS", "searchBooks: expected JSON array");
    return JSON_ERROR;
  }

  JsonArray arr = doc.as<JsonArray>();
  out.count = 0;
  out.currentPage = page;
  out.hasMore = false;

  for (JsonObject book : arr) {
    if (out.count >= BOOKS_PER_PAGE) {
      out.hasMore = true;
      break;
    }

    BookFusionBook& b = out.books[out.count];
    b.id = book["id"] | static_cast<uint32_t>(0);
    if (b.id == 0) continue;

    strlcpy(b.title, book["title"] | "Untitled", sizeof(b.title));
    strlcpy(b.format, book["format"] | "epub", sizeof(b.format));

    // Concatenate author names from the authors array.
    b.authors[0] = '\0';
    JsonArray authors = book["authors"].as<JsonArray>();
    bool first = true;
    for (JsonObject author : authors) {
      const char* name = author["name"] | "";
      if (name[0] != '\0') {
        if (!first) strlcat(b.authors, ", ", sizeof(b.authors));
        strlcat(b.authors, name, sizeof(b.authors));
        first = false;
      }
    }

    out.count++;
  }

  LOG_DBG("BFS", "searchBooks: %d books on page %d, hasMore=%d", out.count, page, out.hasMore);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::getDownloadUrl(uint32_t bookId, char* outUrl, size_t maxLen) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/download", BASE_URL, static_cast<unsigned long>(bookId));

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url);
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST("{}");
  String responseBody = http.getString();
  http.end();

  LOG_DBG("BFS", "getDownloadUrl book=%lu response: %d", static_cast<unsigned long>(bookId), httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 403 || httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) != DeserializationError::Ok) {
    LOG_ERR("BFS", "getDownloadUrl JSON parse error");
    return JSON_ERROR;
  }

  const char* dlUrl = doc["url"] | "";
  if (dlUrl[0] == '\0') {
    LOG_ERR("BFS", "getDownloadUrl: missing url field");
    return JSON_ERROR;
  }

  strlcpy(outUrl, dlUrl, maxLen);
  LOG_DBG("BFS", "getDownloadUrl: ok");
  return OK;
}

const char* BookFusionSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return "Not logged in to BookFusion";
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
    case PENDING:
      return "Authorization pending";
    case SLOW_DOWN:
      return "Slow down polling";
    case EXPIRED:
      return "Device code expired";
    case DENIED:
      return "Authorization denied";
    default:
      return "Unknown error";
  }
}
