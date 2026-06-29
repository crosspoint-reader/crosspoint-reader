#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureClient.h>
#include <WiFiClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const) {}
#endif

namespace {
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

#if defined(FREEINK_NET_WOLFSSL)
struct ParsedUrl {
  std::string scheme;
  std::string host;
  std::string path;
  uint16_t port = 0;
};

bool parseUrl(const std::string& url, ParsedUrl& parsed) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  parsed.scheme = url.substr(0, schemeEnd);
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
  parsed.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    parsed.host = hostPort.substr(0, portSep);
    parsed.port = static_cast<uint16_t>(atoi(hostPort.substr(portSep + 1).c_str()));
  } else {
    parsed.host = hostPort;
    parsed.port = parsed.scheme == "https" ? 443 : 80;
  }
  return !parsed.host.empty() && (parsed.scheme == "http" || parsed.scheme == "https");
}

std::string resolveRedirectUrl(const ParsedUrl& base, const std::string& location) {
  if (location.find("://") != std::string::npos) return location;
  if (!location.empty() && location[0] == '/') {
    return base.scheme + "://" + base.host +
           (base.port == 80 || base.port == 443 ? "" : ":" + std::to_string(base.port)) + location;
  }
  std::string parent = base.path;
  const size_t slash = parent.rfind('/');
  parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
  return base.scheme + "://" + base.host +
         (base.port == 80 || base.port == 443 ? "" : ":" + std::to_string(base.port)) + parent + location;
}

bool readLine(Client& client, std::string& line, const unsigned long deadline) {
  line.clear();
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    while (client.available() > 0) {
      const int c = client.read();
      if (c < 0) break;
      if (c == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      line += static_cast<char>(c);
    }
    if (!client.connected() && client.available() == 0) return false;
    delay(1);
  }
  return false;
}

HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  std::string url = startUrl;
  auto buf = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte wolfSSL read buffer", (unsigned)READ_CHUNK);
    return HttpDownloader::HTTP_ERROR;
  }

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    ParsedUrl parsed;
    if (!parseUrl(url, parsed)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }

    WiFiClient plainClient;
    freeink::SecureClient secureClient;
    Client* client = nullptr;
    if (parsed.scheme == "https") {
      secureClient.setInsecure();
      client = &secureClient;
      LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    } else {
      client = &plainClient;
    }
    client->setTimeout(HTTP_TIMEOUT_MS / 1000);
    if (!client->connect(parsed.host.c_str(), parsed.port)) {
      LOG_ERR("HTTP", "wolfSSL connect failed: %s:%u", parsed.host.c_str(), parsed.port);
      return HttpDownloader::HTTP_ERROR;
    }

    std::string request = "GET " + parsed.path + " HTTP/1.1\r\nHost: " + parsed.host +
                          "\r\nUser-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION "\r\nConnection: close\r\n";
    if (!username.empty() && !password.empty()) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      request += "Authorization: Basic " + std::string(encoded.c_str()) + "\r\n";
    }
    request += "\r\n";
    client->write(reinterpret_cast<const uint8_t*>(request.c_str()), request.size());

    const unsigned long headerDeadline = millis() + HTTP_TIMEOUT_MS;
    std::string line;
    if (!readLine(*client, line, headerDeadline)) {
      LOG_ERR("HTTP", "wolfSSL no status line");
      client->stop();
      return HttpDownloader::HTTP_ERROR;
    }
    const int status = line.size() >= 12 ? atoi(line.c_str() + 9) : 0;
    size_t contentLength = 0;
    std::string location;
    while (readLine(*client, line, headerDeadline)) {
      if (line.empty()) break;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return static_cast<char>(tolower(c)); });
      if (name == "content-length") contentLength = static_cast<size_t>(strtoul(value.c_str(), nullptr, 10));
      if (name == "location") location = value;
    }

    if (isRedirect(status) && !location.empty()) {
      url = resolveRedirectUrl(parsed, location);
      client->stop();
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      client->stop();
      return HttpDownloader::HTTP_ERROR;
    }

    sink.total = contentLength;
    unsigned long readDeadline = millis() + HTTP_TIMEOUT_MS;
    while (sink.total == 0 || sink.downloaded < sink.total) {
      if (sink.cancelFlag && *sink.cancelFlag) {
        client->stop();
        return HttpDownloader::ABORTED;
      }
      if (client->available() <= 0) {
        if (!client->connected()) break;
        if (static_cast<int32_t>(millis() - readDeadline) >= 0) {
          LOG_ERR("HTTP", "wolfSSL read timeout after %zu bytes", sink.downloaded);
          client->stop();
          return HttpDownloader::HTTP_ERROR;
        }
        delay(1);
        continue;
      }
      const int read = client->read(buf.get(), READ_CHUNK);
      if (read <= 0) {
        // SecureClient exposes wolfSSL WANT_READ/WANT_WRITE as a non-positive
        // Client::read() result. Keep polling until data, close, or timeout.
        if (!client->connected() && client->available() == 0) break;
        if (static_cast<int32_t>(millis() - readDeadline) >= 0) {
          LOG_ERR("HTTP", "wolfSSL read timeout after %zu bytes", sink.downloaded);
          client->stop();
          return HttpDownloader::HTTP_ERROR;
        }
        delay(2);
        continue;
      }
      readDeadline = millis() + HTTP_TIMEOUT_MS;
      if (!sink.write(buf.get(), static_cast<size_t>(read))) {
        client->stop();
        return HttpDownloader::FILE_ERROR;
      }
      sink.downloaded += static_cast<size_t>(read);
      if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
    }
    client->stop();
    if (sink.total > 0 && sink.downloaded != sink.total) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}
#endif

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result =
#if defined(FREEINK_NET_WOLFSSL)
      runGetWolf(url, username, password, sink);
#else
      runGet(url, username, password, sink);
#endif
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
