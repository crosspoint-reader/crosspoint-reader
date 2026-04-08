#include "HardcoverClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFiClientSecure.h>

#include <memory>

#include "HardcoverCredentialStore.h"

// Forward-declare esp_crt_bundle_attach without including esp_crt_bundle.h, which
// conflicts with the Arduino WiFiClientSecure header in this build environment
// (same approach as OtaUpdater.cpp).
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

namespace {
constexpr char API_URL[] = "https://api.hardcover.app/v1/graphql";
constexpr char LOG_TAG[] = "HC";

/// Execute a GraphQL request and parse the JSON response.
/// @param body  The full JSON request body ({"query":"..."})
/// @param outDoc  Output: parsed JSON response document
/// @return HardcoverClient::Error code
HardcoverClient::Error executeGraphQL(const char* body, JsonDocument& outDoc) {
  // Snapshot the token once to avoid a race with concurrent setToken()/clearToken()
  const std::string token = HARDCOVER_STORE.getToken();
  if (token.empty()) {
    LOG_DBG(LOG_TAG, "No token configured");
    return HardcoverClient::NO_TOKEN;
  }

  HTTPClient http;
  auto secureClient = std::make_unique<WiFiClientSecure>();
  if (!secureClient) {
    LOG_ERR(LOG_TAG, "Failed to allocate WiFiClientSecure");
    return HardcoverClient::NETWORK_ERROR;
  }
  // Use the built-in Mozilla CA certificate bundle for TLS verification.
  // Requires the device clock to be synchronised (NTP) so certificate dates
  // pass.  Uses the same esp_crt_bundle_attach extern as OtaUpdater.cpp.
  secureClient->setCACertBundle(esp_crt_bundle_attach);
  http.begin(*secureClient, API_URL);

  // Auth and content headers
  char authHeader[256];
  const int authWritten = snprintf(authHeader, sizeof(authHeader), "Bearer %s", token.c_str());
  if (authWritten < 0 || authWritten >= (int)sizeof(authHeader)) {
    LOG_ERR(LOG_TAG, "Bearer token too long for auth header buffer");
    return HardcoverClient::AUTH_FAILED;
  }
  http.addHeader("Authorization", authHeader);
  http.addHeader("Content-Type", "application/json");

  LOG_DBG(LOG_TAG, "POST %s (%d bytes)", API_URL, (int)strlen(body));

  const int httpCode = http.POST(body);

  if (httpCode < 0) {
    http.end();
    LOG_ERR(LOG_TAG, "Network error: %d", httpCode);
    return HardcoverClient::NETWORK_ERROR;
  }

  if (httpCode == 401 || httpCode == 403) {
    http.end();
    LOG_ERR(LOG_TAG, "Auth failed: %d", httpCode);
    return HardcoverClient::AUTH_FAILED;
  }

  if (httpCode == 429) {
    http.end();
    LOG_ERR(LOG_TAG, "Rate limited");
    return HardcoverClient::RATE_LIMITED;
  }

  if (httpCode != 200) {
    http.end();
    LOG_ERR(LOG_TAG, "Server error: %d", httpCode);
    return HardcoverClient::SERVER_ERROR;
  }

  // Parse JSON directly from the HTTP stream to avoid an intermediate
  // Arduino String allocation (per String Policy in SKILL.md).
  const DeserializationError err = deserializeJson(outDoc, http.getStream());
  http.end();

  if (err) {
    LOG_ERR(LOG_TAG, "JSON parse failed: %s", err.c_str());
    return HardcoverClient::JSON_ERROR;
  }

  // Check for GraphQL-level errors
  if (outDoc["errors"].is<JsonArray>() && outDoc["errors"].as<JsonArray>().size() > 0) {
    const char* msg = outDoc["errors"][0]["message"] | "unknown";
    LOG_ERR(LOG_TAG, "GraphQL error: %s", msg);
    return HardcoverClient::SERVER_ERROR;
  }

  return HardcoverClient::OK;
}
}  // namespace

HardcoverClient::Error HardcoverClient::authenticate() {
  // Build a minimal "me" query to validate the token
  static constexpr char QUERY[] = R"({"query":"{ me { id username } }"})";

  JsonDocument doc;
  Error err = executeGraphQL(QUERY, doc);
  if (err != OK) return err;

  int userId = doc["data"]["me"]["id"] | 0;
  const char* username = doc["data"]["me"]["username"] | "unknown";
  if (userId == 0) {
    LOG_ERR(LOG_TAG, "Auth response missing user ID");
    return JSON_ERROR;
  }

  LOG_INF(LOG_TAG, "Authenticated as %s (id=%d)", username, userId);
  return OK;
}

HardcoverClient::Error HardcoverClient::searchBook(const char* query, int& outBookId) {
  outBookId = 0;

  // Build request body with JSON serialization so `query` is properly escaped
  // and supplied as GraphQL data rather than interpolated into the query text.
  JsonDocument requestBodyDoc;
  requestBodyDoc["query"] =
      "query SearchBooks($query: String!) { "
      "search(query: $query, query_type: \"books\") { "
      "results { ... on Book { id title } } "
      "} "
      "}";
  requestBodyDoc["variables"]["query"] = query;

  String body;
  if (serializeJson(requestBodyDoc, body) == 0 || body.length() >= 512) {
    LOG_ERR(LOG_TAG, "Search query too long");
    return SERVER_ERROR;
  }

  JsonDocument doc;
  Error err = executeGraphQL(body.c_str(), doc);
  if (err != OK) return err;

  JsonArray results = doc["data"]["search"]["results"];
  if (!results || results.size() == 0) {
    LOG_DBG(LOG_TAG, "No results for: %s", query);
    return NOT_FOUND;
  }

  outBookId = results[0]["id"] | 0;
  const char* title = results[0]["title"] | "unknown";
  LOG_DBG(LOG_TAG, "Found book: %s (id=%d)", title, outBookId);

  if (outBookId == 0) return NOT_FOUND;
  return OK;
}

HardcoverClient::Error HardcoverClient::addBook(int bookId, int statusId, int& outUserBookId) {
  outUserBookId = 0;

  char body[256];
  int written =
      snprintf(body, sizeof(body),
               R"({"query":"mutation { insert_user_books_one(object: { book_id: %d, status_id: %d }) { id } }"})",
               bookId, statusId);
  if (written < 0 || written >= (int)sizeof(body)) {
    LOG_ERR(LOG_TAG, "addBook query too long");
    return SERVER_ERROR;
  }

  JsonDocument doc;
  Error err = executeGraphQL(body, doc);
  if (err != OK) return err;

  outUserBookId = doc["data"]["insert_user_books_one"]["id"] | 0;
  if (outUserBookId == 0) {
    LOG_ERR(LOG_TAG, "addBook: missing user_book id in response");
    return JSON_ERROR;
  }

  LOG_DBG(LOG_TAG, "Added book %d with status %d → user_book %d", bookId, statusId, outUserBookId);
  return OK;
}

HardcoverClient::Error HardcoverClient::updateStatus(int userBookId, int statusId) {
  char body[256];
  int written =
      snprintf(body, sizeof(body),
               R"({"query":"mutation { update_user_books_by_pk(pk_columns: { id: %d }, _set: { status_id: %d }) { id } }"})",
               userBookId, statusId);
  if (written < 0 || written >= (int)sizeof(body)) {
    LOG_ERR(LOG_TAG, "updateStatus query too long");
    return SERVER_ERROR;
  }

  JsonDocument doc;
  Error err = executeGraphQL(body, doc);
  if (err != OK) return err;

  int updatedId = doc["data"]["update_user_books_by_pk"]["id"] | 0;
  if (updatedId == 0) {
    LOG_ERR(LOG_TAG, "updateStatus: missing id in response");
    return SERVER_ERROR;
  }

  LOG_DBG(LOG_TAG, "Updated user_book %d → status %d", userBookId, statusId);
  return OK;
}

HardcoverClient::Error HardcoverClient::updateProgress(int userBookId, float progressPercent) {
  // Clamp to valid range
  if (progressPercent < 0.0f) progressPercent = 0.0f;
  if (progressPercent > 1.0f) progressPercent = 1.0f;

  int percentInt = static_cast<int>(progressPercent * 100.0f);

  char body[256];
  int written = snprintf(
      body, sizeof(body),
      R"({"query":"mutation { insert_user_book_reads_one(object: { user_book_id: %d, progress_percentage: %d }) { id } }"})",
      userBookId, percentInt);
  if (written < 0 || written >= (int)sizeof(body)) {
    LOG_ERR(LOG_TAG, "updateProgress query too long");
    return SERVER_ERROR;
  }

  JsonDocument doc;
  Error err = executeGraphQL(body, doc);
  if (err != OK) return err;

  int insertedId = doc["data"]["insert_user_book_reads_one"]["id"] | 0;
  if (insertedId == 0) {
    LOG_ERR(LOG_TAG, "updateProgress: missing id in response");
    return SERVER_ERROR;
  }

  LOG_DBG(LOG_TAG, "Updated progress: user_book %d → %d%%", userBookId, percentInt);
  return OK;
}

const char* HardcoverClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return "No API token configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case RATE_LIMITED:
      return "Rate limited (try again later)";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "Not found";
    default:
      return "Unknown error";
  }
}
