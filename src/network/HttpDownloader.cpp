#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <strings.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

esp_err_t captureLocationHeader(esp_http_client_event_t* evt) {
  auto* location = static_cast<std::string*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && location != nullptr && evt->header_key != nullptr &&
      evt->header_value != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
    location->append(evt->header_value);
  }
  return ESP_OK;
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

struct ParsedUrl {
  bool https = false;
  std::string host;
  std::string path;
  uint16_t port = 80;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  const std::string scheme = url.substr(0, schemeEnd);
  out.https = scheme == "https";
  if (!out.https && scheme != "http") return false;

  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  out.port = out.https ? 443 : 80;

  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    out.host = hostPort.substr(0, portSep);
    out.port = static_cast<uint16_t>(atoi(hostPort.substr(portSep + 1).c_str()));
  } else {
    out.host = hostPort;
  }
  return !out.host.empty() && !out.path.empty();
}

bool isGitHubHost(const std::string& host) {
  static constexpr const char* GITHUBUSERCONTENT_SUFFIX = ".githubusercontent.com";
  const size_t suffixLen = strlen(GITHUBUSERCONTENT_SUFFIX);
  return host == "github.com" || host == "gist.github.com" || host == "raw.githubusercontent.com" ||
         host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com" ||
         (host.size() > suffixLen && host.compare(host.size() - suffixLen, suffixLen, GITHUBUSERCONTENT_SUFFIX) == 0);
}

bool useMinimalClientForUrl(const std::string& url) {
  ParsedUrl parsed;
  return parseUrl(url, parsed) && parsed.https && isGitHubHost(parsed.host);
}

bool sameOrigin(const ParsedUrl& a, const ParsedUrl& b) {
  return a.https == b.https && a.port == b.port && strcasecmp(a.host.c_str(), b.host.c_str()) == 0;
}

std::string buildRedirectUrl(const std::string& baseUrl, const std::string& location) {
  if (location.find("http://") == 0 || location.find("https://") == 0) return location;

  ParsedUrl base;
  if (!parseUrl(baseUrl, base)) return location;

  std::string origin = base.https ? "https://" : "http://";
  origin += base.host;
  if ((base.https && base.port != 443) || (!base.https && base.port != 80)) {
    origin += ":";
    origin += std::to_string(base.port);
  }

  if (!location.empty() && location[0] == '/') return origin + location;

  const size_t lastSlash = base.path.rfind('/');
  const std::string parent = lastSlash == std::string::npos ? "/" : base.path.substr(0, lastSlash + 1);
  return origin + parent + location;
}

bool readLine(NetworkClient& client, std::string& out, size_t maxLen = 2048) {
  out.clear();
  const uint32_t start = millis();
  while (millis() - start < HTTP_TIMEOUT_MS) {
    while (client.available() > 0) {
      const int c = client.read();
      if (c < 0) return false;
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      if (out.size() < maxLen) out.push_back(static_cast<char>(c));
    }
    if (!client.connected()) return false;
    delay(1);
  }
  return false;
}

bool headerStartsWith(const std::string& line, const char* name) {
  const size_t nameLen = strlen(name);
  return line.size() > nameLen && line[nameLen] == ':' && strncasecmp(line.c_str(), name, nameLen) == 0;
}

std::string headerValue(const std::string& line) {
  const size_t sep = line.find(':');
  if (sep == std::string::npos) return "";
  size_t start = sep + 1;
  while (start < line.size() && line[start] == ' ') start++;
  return line.substr(start);
}

HttpDownloader::DownloadError runGetWithMinimalClient(const std::string& url, Sink& sink) {
  std::string currentUrl = url;
  for (int hop = 0; hop < 5; ++hop) {
    ParsedUrl parsed;
    if (!parseUrl(currentUrl, parsed)) {
      LOG_ERR("HTTP", "bad URL");
      return HttpDownloader::HTTP_ERROR;
    }

    std::unique_ptr<NetworkClient> clientHolder;
    if (parsed.https) {
      auto secureClient = makeUniqueNoThrow<NetworkClientSecure>();
      if (!secureClient) {
        LOG_ERR("HTTP", "OOM: client");
        return HttpDownloader::HTTP_ERROR;
      }
      secureClient->setInsecure();
      clientHolder = std::move(secureClient);
    } else {
      clientHolder = makeUniqueNoThrow<NetworkClient>();
      if (!clientHolder) {
        LOG_ERR("HTTP", "OOM: client");
        return HttpDownloader::HTTP_ERROR;
      }
    }
    NetworkClient* client = clientHolder.get();
    client->setTimeout(HTTP_TIMEOUT_MS);

    if (!client->connect(parsed.host.c_str(), parsed.port, HTTP_TIMEOUT_MS)) {
      LOG_ERR("HTTP", "connect failed: %s", parsed.host.c_str());
      return HttpDownloader::HTTP_ERROR;
    }

    client->print("GET ");
    client->print(parsed.path.c_str());
    client->print(" HTTP/1.1\r\nHost: ");
    client->print(parsed.host.c_str());
    client->print("\r\nUser-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION "\r\nConnection: close\r\n\r\n");

    std::string line;
    if (!readLine(*client, line)) {
      LOG_ERR("HTTP", "missing status");
      client->stop();
      return HttpDownloader::HTTP_ERROR;
    }
    const int status = line.size() >= 12 ? atoi(line.c_str() + 9) : 0;

    size_t contentLength = 0;
    std::string location;
    bool chunked = false;
    while (readLine(*client, line)) {
      if (line.empty()) break;
      if (headerStartsWith(line, "Content-Length")) {
        contentLength = static_cast<size_t>(atol(headerValue(line).c_str()));
      } else if (headerStartsWith(line, "Location")) {
        location = headerValue(line);
      } else if (headerStartsWith(line, "Transfer-Encoding") &&
                 headerValue(line).find("chunked") != std::string::npos) {
        chunked = true;
      }
    }

    if (isRedirect(status)) {
      client->stop();
      if (location.empty()) break;
      currentUrl = std::move(location);
      continue;
    }

    if (status != 200 || chunked) {
      LOG_ERR("HTTP", "unexpected status: %d", status);
      client->stop();
      return HttpDownloader::HTTP_ERROR;
    }

    sink.total = contentLength;

    auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
    if (!buf) {
      LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
      client->stop();
      return HttpDownloader::HTTP_ERROR;
    }

    while (client->connected() && (sink.total == 0 || sink.downloaded < sink.total)) {
      if (sink.cancelFlag && *sink.cancelFlag) {
        client->stop();
        return HttpDownloader::ABORTED;
      }

      const int available = client->available();
      if (available <= 0) {
        delay(1);
        continue;
      }

      const size_t toRead = std::min(static_cast<size_t>(available), READ_CHUNK);
      const int read = client->readBytes(buf.get(), toRead);
      if (read <= 0) {
        LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
        client->stop();
        return HttpDownloader::HTTP_ERROR;
      }
      if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
        client->stop();
        return HttpDownloader::FILE_ERROR;
      }
      sink.downloaded += read;
      if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
    }

    client->stop();
    if (sink.total > 0 && sink.downloaded != sink.total) {
      LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "redirect failed");
  return HttpDownloader::HTTP_ERROR;
}

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  if (username.empty() && password.empty() && useMinimalClientForUrl(url)) {
    return runGetWithMinimalClient(url, sink);
  }

  std::string currentUrl = url;
  ParsedUrl credentialOrigin;
  const bool hasCredentials = !username.empty() && !password.empty() && parseUrl(url, credentialOrigin);
  for (int hop = 0; hop < 5; ++hop) {
    ParsedUrl currentOrigin;
    const bool sendAuthorization =
        hasCredentials && parseUrl(currentUrl, currentOrigin) && sameOrigin(currentOrigin, credentialOrigin);
    std::string redirectLocation;
    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
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
    config.keep_alive_enable = false;
    config.event_handler = captureLocationHeader;
    config.user_data = &redirectLocation;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
      return HttpDownloader::HTTP_ERROR;
    }

    esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
    esp_http_client_set_header(client, "Connection", "close");
    if (sendAuthorization) {
      // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
      const std::string credentials = username + ":" + password;
      const String header = "Basic " + base64::encode(credentials.c_str());
      esp_http_client_set_header(client, "Authorization", header.c_str());
    }

    const esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    const int64_t contentLength = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (contentLength < 0 && status < 0) {
      LOG_ERR("HTTP", "fetch headers failed");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      if (redirectLocation.empty()) {
        LOG_ERR("HTTP", "redirect missing location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = buildRedirectUrl(currentUrl, redirectLocation);
      ParsedUrl redirect;
      if (parseUrl(currentUrl, redirect)) {
        LOG_DBG("HTTP", "Redirecting to: %s", redirect.host.c_str());
      }
      esp_http_client_cleanup(client);
      continue;
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
      if (sink.total > 0 && sink.downloaded >= sink.total) break;
    }

    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    if (!complete) {
      LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "redirect failed");
  return HttpDownloader::HTTP_ERROR;
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

  const DownloadError result = runGet(url, username, password, sink);
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
