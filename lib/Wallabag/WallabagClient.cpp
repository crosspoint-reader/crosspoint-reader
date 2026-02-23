#include "WallabagClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "WallabagCredentialStore.h"

namespace {
// Stream wrapper that yields to the WiFi/LwIP FreeRTOS task between reads.
// On the single-core ESP32-C3, Stream::timedRead() is a tight busy-wait that
// starves the WiFi task from processing incoming TCP segments and decrypting
// TLS records, causing ArduinoJson to see a dry stream and return IncompleteInput.
// Calling delay(1) here yields via vTaskDelay, letting the WiFi task run.
class BlockingWiFiStream : public Stream {
 public:
  explicit BlockingWiFiStream(WiFiClient& client, unsigned long timeoutMs = 15000)
      : _client(client), _timeoutMs(timeoutMs) {}

  int read() override {
    const unsigned long start = millis();
    while (millis() - start < _timeoutMs) {
      int c = _client.read();
      if (c >= 0) return c;
      if (!_client.connected()) return -1;  // Connection closed, no more data
      delay(1);                              // Yield to WiFi/LwIP FreeRTOS task
    }
    return -1;  // Timeout
  }

  int available() override { return _client.available(); }
  size_t write(uint8_t) override { return 0; }
  int peek() override { return _client.peek(); }
  void flush() override {}

 private:
  WiFiClient& _client;
  unsigned long _timeoutMs;
};

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

// Normalize URL: add https:// if no protocol specified
std::string normalizeUrl(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "https://" + url;
  }
  return url;
}

void beginRequest(HTTPClient& http, WiFiClient& plainClient, WiFiClientSecure& secureClient,
                  const std::string& url) {
  if (isHttpsUrl(url)) {
    secureClient.setInsecure();
    http.begin(secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
}
}  // namespace

WallabagClient::Error WallabagClient::authenticate() {
  if (!WALLABAG_STORE.hasCredentials()) {
    LOG_DBG("WBG", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string base = normalizeUrl(WALLABAG_STORE.getServerUrl());
  const std::string url = base + "/oauth/v2/token";
  LOG_DBG("WBG", "Authenticating: %s", url.c_str());

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  beginRequest(http, plainClient, secureClient, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // Build form body
  std::string body = "grant_type=password";
  body += "&client_id=" + WALLABAG_STORE.getClientId();
  body += "&client_secret=" + WALLABAG_STORE.getClientSecret();
  body += "&username=" + WALLABAG_STORE.getUsername();
  body += "&password=" + WALLABAG_STORE.getPassword();

  const int httpCode = http.POST(body.c_str());

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, responseBody)) {
      LOG_ERR("WBG", "JSON parse failed for token response");
      return JSON_ERROR;
    }

    const char* token = doc["access_token"];
    const int64_t expiresIn = doc["expires_in"].as<int64_t>();
    if (!token || expiresIn <= 0) {
      LOG_ERR("WBG", "Invalid token response");
      return SERVER_ERROR;
    }

    WALLABAG_STORE.storeToken(std::string(token), expiresIn);
    LOG_DBG("WBG", "Authentication successful");
    return OK;
  }

  http.end();
  LOG_DBG("WBG", "Auth response: %d", httpCode);

  if (httpCode == 400 || httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

WallabagClient::Error WallabagClient::fetchArticles(std::vector<WallabagArticle>& out, int limit) {
  if (!WALLABAG_STORE.hasCredentials()) return NO_CREDENTIALS;

  if (!WALLABAG_STORE.isTokenValid()) {
    const Error authErr = authenticate();
    if (authErr != OK) return authErr;
  }

  // Use limit 0 to mean "fetch a large number"
  const int perPage = (limit > 0) ? limit : 100;

  const std::string base = normalizeUrl(WALLABAG_STORE.getServerUrl());
  std::string url = base + "/api/entries.json?archive=0&page=1&perPage=" + std::to_string(perPage);
  url += "&sort=updated&order=desc";
  LOG_DBG("WBG", "Fetching articles: %s", url.c_str());

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  beginRequest(http, plainClient, secureClient, url);
  http.addHeader("Authorization", ("Bearer " + WALLABAG_STORE.getAccessToken()).c_str());
  // Force HTTP/1.0: server closes connection after body instead of using chunked
  // transfer-encoding. ArduinoJson's stream parser uses TCP connection-close as EOF;
  // without this, momentary gaps in TCP data look like EOF and cut the parse short.
  http.useHTTP10(true);

  const int httpCode = http.GET();

  if (httpCode == 200) {
    // Use a filter to discard all fields except the three we need.
    // This dramatically reduces the memory required for parsing.
    JsonDocument filter;
    filter["_embedded"]["items"][0]["id"] = true;
    filter["_embedded"]["items"][0]["title"] = true;
    filter["_embedded"]["items"][0]["updated_at"] = true;

    JsonDocument doc;
    // Parse directly from the HTTP stream — no intermediate String buffer.
    WiFiClient* rawStream = http.getStreamPtr();
    DeserializationError err = DeserializationError::Ok;
    if (rawStream) {
      BlockingWiFiStream stream(*rawStream);
      err = deserializeJson(doc, stream, DeserializationOption::Filter(filter));
    } else {
      err = DeserializationError::EmptyInput;
    }
    http.end();

    if (err) {
      LOG_ERR("WBG", "JSON parse failed for entries response: %s", err.c_str());
      return JSON_ERROR;
    }

    out.clear();
    JsonArray items = doc["_embedded"]["items"].as<JsonArray>();
    for (JsonObject item : items) {
      WallabagArticle article;
      article.id = item["id"].as<int>();
      const char* title = item["title"];
      article.title = title ? title : "(Untitled)";
      article.updatedAt = item["id"].as<int64_t>();  // API already sorted by updated desc
      out.push_back(std::move(article));
    }

    LOG_DBG("WBG", "Fetched %d articles", (int)out.size());
    return OK;
  }

  http.end();
  LOG_DBG("WBG", "Entries response: %d", httpCode);

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

WallabagClient::Error WallabagClient::downloadArticle(int id, const std::string& destPath,
                                                      std::function<void(size_t, size_t)> progress) {
  if (!WALLABAG_STORE.hasCredentials()) return NO_CREDENTIALS;

  if (!WALLABAG_STORE.isTokenValid()) {
    const Error authErr = authenticate();
    if (authErr != OK) return authErr;
  }

  const std::string base = normalizeUrl(WALLABAG_STORE.getServerUrl());
  const std::string url = base + "/api/entries/" + std::to_string(id) + "/export.epub";
  LOG_DBG("WBG", "Downloading article %d -> %s", id, destPath.c_str());

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  beginRequest(http, plainClient, secureClient, url);
  http.addHeader("Authorization", ("Bearer " + WALLABAG_STORE.getAccessToken()).c_str());
  // Force HTTP/1.0 so server closes connection after body, giving us a proper EOF.
  // Under HTTP/1.1 keep-alive the connection stays open and http.connected() never
  // returns false, causing the download loop below to spin forever.
  http.useHTTP10(true);

  const int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    LOG_DBG("WBG", "Download response: %d", httpCode);
    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode < 0) return NETWORK_ERROR;
    return SERVER_ERROR;
  }

  // Ensure /Articles directory exists
  Storage.mkdir("/Articles");

  FsFile outFile;
  if (!Storage.openFileForWrite("WBG", destPath, outFile)) {
    http.end();
    LOG_ERR("WBG", "Failed to open file for writing: %s", destPath.c_str());
    return SERVER_ERROR;
  }

  const int contentLength = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    outFile.close();
    http.end();
    return NETWORK_ERROR;
  }

  constexpr size_t CHUNK = 1024;
  uint8_t buf[CHUNK];
  size_t downloaded = 0;
  const size_t total = (contentLength > 0) ? static_cast<size_t>(contentLength) : 0;

  while (http.connected() && (contentLength < 0 || downloaded < total)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }
    const size_t toRead = (available < CHUNK) ? available : CHUNK;
    const size_t read = stream->readBytes(buf, toRead);
    if (read == 0) break;
    outFile.write(buf, read);
    downloaded += read;
    if (progress) progress(downloaded, total);
  }

  outFile.close();
  http.end();

  LOG_DBG("WBG", "Downloaded %d bytes", (int)downloaded);
  return (downloaded > 0) ? OK : NETWORK_ERROR;
}

const char* WallabagClient::errorString(Error error) {
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
      return "Server error";
    case JSON_ERROR:
      return "JSON parse error";
    default:
      return "Unknown error";
  }
}
