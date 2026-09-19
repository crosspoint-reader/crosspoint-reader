#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_wifi.h>
#include <strings.h>

#include <functional>
#include <string>

#include "util/UrlUtils.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;
// Mid-stream drops on multi-MB books are routine: signed OPDS download URLs
// (CDN redirect tokens) expire after a few minutes, which is shorter than a
// large transfer takes. Each resume attempt re-follows the original URL for a
// fresh token and continues with a Range request.
constexpr int MAX_RESUME_ATTEMPTS = 8;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  // Called when a resumed request comes back 200 instead of 206 (server
  // ignored Range): rewind the destination so the full body restarts cleanly.
  std::function<bool()> restart;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
  // Byte offset to resume from; 0 requests the whole body.
  size_t resumeOffset = 0;
  // Full Authorization header value ("Basic ..." / "Bearer ..."); empty sends none.
  std::string authorization;
  // Accept header for content negotiation; empty sends none.
  std::string accept;
  // Non-null turns the request into a form-urlencoded POST of this body.
  const std::string* postBody = nullptr;
  // Also stream a 401 body to `write` (OPDS auth documents are 401 bodies).
  bool captureErrorBody = false;
  int status = 0;  // final HTTP status (0 if no response)
};

std::string buildAuthHeader(const std::string& username, const std::string& password, const std::string& bearer) {
  if (!bearer.empty()) return "Bearer " + bearer;
  if (username.empty() || password.empty()) return "";
  const String encoded = base64::encode((username + ":" + password).c_str());
  return std::string("Basic ") + encoded.c_str();
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// OtaUpdater.cpp already disables WiFi power-save for firmware downloads, but
// OPDS feed/book fetches never did despite being able to run just as long for
// a large category. Modem sleep periodically powers the radio down between
// DTIM beacon intervals, which can drop or stall packets mid-transfer -- more
// likely to be hit the longer a transfer takes, so small feeds mostly get
// away with it while a large category consistently doesn't.
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to disable WiFi power-save: %d", err);
  }
  ~WifiPowerSaveGuard() {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to restore WiFi power-save: %d", err);
  }
};

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, Sink& sink, bool downgradeRedirectsToHttp) {
  WifiPowerSaveGuard psGuard;
  std::string url = startUrl;
  // Credentials belong to the configured server only: a redirect to another
  // origin (a CDN, or an http-downgraded target) must not receive them.
  const std::string startOrigin = UrlUtils::extractHost(UrlUtils::ensureProtocol(startUrl));

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
    // append a second User-Agent header, which strict servers reject (aiohttp
    // answers 400 "Duplicate 'User-Agent' header found").
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!sink.authorization.empty() &&
        strcasecmp(UrlUtils::extractHost(UrlUtils::ensureProtocol(url)).c_str(), startOrigin.c_str()) == 0) {
      http.addHeader("Authorization", sink.authorization);
    }
    if (!sink.accept.empty()) {
      http.addHeader("Accept", sink.accept);
    }
    if (sink.resumeOffset > 0) {
      char range[48];
      snprintf(range, sizeof(range), "bytes=%zu-", sink.resumeOffset);
      http.addHeader("Range", range);
    }
    if (sink.postBody) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    }

    LOG_DBG("HTTP", "wolfSSL %s: %s", sink.postBody ? "POST" : "GET", url.c_str());
    const auto onData = [&http, &sink](const uint8_t* data, size_t len) {
      const int st = http.getStatus();
      const bool errCapture = sink.captureErrorBody && st == 401;
      if (st != 200 && st != 206 && !errCapture) return true;
      if (st == 200 && sink.resumeOffset > 0) {
        // Server ignored the Range request; restart the body from zero.
        if (!sink.restart || !sink.restart()) return false;
        sink.resumeOffset = 0;
        sink.downloaded = 0;
        sink.total = 0;
      }
      if (sink.total == 0 && http.hasContentLength()) sink.total = sink.resumeOffset + http.getContentLength();
      if (!sink.write(data, len)) return false;
      sink.downloaded += len;
      if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
      return true;
    };
    const auto shouldAbort = [&sink]() { return sink.cancelFlag && *sink.cancelFlag; };
    const int status = sink.postBody ? http.sendRequest("POST", reinterpret_cast<const uint8_t*>(sink.postBody->data()),
                                                        sink.postBody->size(), onData, shouldAbort)
                                     : http.GET(onData, shouldAbort);
    sink.status = status > 0 ? status : 0;

    if (http.aborted()) return HttpDownloader::ABORTED;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      if (sink.postBody) {
        LOG_ERR("HTTP", "wolfSSL unexpected POST redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      if (downgradeRedirectsToHttp && url.rfind("https://", 0) == 0) {
        // Fetch the redirect target over plain HTTP. GitHub's release-asset
        // CDN serves its signed URLs on both schemes, and skipping the second
        // TLS session removes its ~17KB record buffer — the MEMORY_E /
        // OOM-abort site on C3 heaps that sit near 45KB free.
        url.replace(0, 8, "http://");
      }
      continue;
    }
    if (status != 200 && status != 206) {
      if (!(sink.captureErrorBody && status == 401)) LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}
#endif

#if !defined(FREEINK_NET_WOLFSSL)
// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, Sink& sink) {
  WifiPowerSaveGuard psGuard;
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
  if (!sink.authorization.empty()) {
    // Preemptive auth (Basic or Bearer); don't wait for a 401.
    esp_http_client_set_header(client, "Authorization", sink.authorization.c_str());
  }
  if (!sink.accept.empty()) {
    esp_http_client_set_header(client, "Accept", sink.accept.c_str());
  }
  if (sink.resumeOffset > 0) {
    char range[48];
    snprintf(range, sizeof(range), "bytes=%zu-", sink.resumeOffset);
    esp_http_client_set_header(client, "Range", range);
  }

  const size_t postLen = sink.postBody ? sink.postBody->size() : 0;
  if (sink.postBody) {
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, postLen);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  if (sink.postBody &&
      esp_http_client_write(client, sink.postBody->data(), static_cast<int>(postLen)) != static_cast<int>(postLen)) {
    LOG_ERR("HTTP", "POST body write failed");
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && !sink.postBody && hop < MAX_REDIRECTS; ++hop) {
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

  sink.status = status;
  const bool errCapture = sink.captureErrorBody && status == 401;
  if (status != 200 && status != 206 && !errCapture) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  if (status == 200 && sink.resumeOffset > 0) {
    // Server ignored the Range request; restart the body from zero.
    if (!sink.restart || !sink.restart()) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.resumeOffset = 0;
    sink.downloaded = 0;
    sink.total = 0;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  if (sink.total == 0) sink.total = contentLength > 0 ? sink.resumeOffset + static_cast<size_t>(contentLength) : 0;

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
  if (errCapture) return HttpDownloader::HTTP_ERROR;  // 401 body was streamed for the caller
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
#endif  // !FREEINK_NET_WOLFSSL

// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
HttpDownloader::DownloadError runGetSecure(const std::string& url, Sink& sink, bool downgradeRedirectsToHttp = false) {
#if defined(FREEINK_NET_WOLFSSL)
  return runGetWolf(url, sink, downgradeRedirectsToHttp);
#else
  // esp_http_client follows redirects internally; the downgrade only exists on
  // the wolfSSL path, where the manual hop loop exposes the Location URL.
  (void)downgradeRedirectsToHttp;
  return runGet(url, sink);
#endif
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  sink.authorization = buildAuthHeader(username, password, "");
  return runGetSecure(url, sink) == OK;
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
  sink.authorization = buildAuthHeader(username, password, "");
  return runGetSecure(url, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  sink.authorization = buildAuthHeader(username, password, "");
  return runGetSecure(url, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const FetchOptions& options) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  sink.authorization = buildAuthHeader(options.username, options.password, options.bearer);
  sink.accept = options.accept;
  sink.captureErrorBody = options.captureErrorBody;
  const bool ok = runGetSecure(url, sink) == OK;
  if (options.statusOut) *options.statusOut = sink.status;
  return ok;
}

bool HttpDownloader::postForm(const std::string& url, const std::string& formBody, std::string& outResponse,
                              int* statusOut) {
  LOG_DBG("HTTP", "POST: %s", url.c_str());
  // Token responses are small JSON documents; cap the capture so a
  // misbehaving endpoint can't balloon the heap.
  constexpr size_t MAX_POST_RESPONSE = 4096;
  outResponse.clear();
  Sink sink;
  sink.postBody = &formBody;
  sink.captureErrorBody = true;  // an OAuth error body is still useful to log
  sink.write = [&outResponse](const uint8_t* data, size_t len) {
    const size_t room = MAX_POST_RESPONSE - outResponse.size();
    outResponse.append(reinterpret_cast<const char*>(data), len < room ? len : room);
    return true;
  };
  const bool ok = runGetSecure(url, sink) == OK;
  if (statusOut) *statusOut = sink.status;
  return ok && sink.status >= 200 && sink.status < 300;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             bool downgradeRedirectsToHttp, const std::string& bearer) {
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
  sink.restart = [&file, &destPath]() {
    file.close();
    Storage.remove(destPath.c_str());
    return Storage.openFileForWrite("HTTP", destPath.c_str(), file);
  };
  sink.authorization = buildAuthHeader(username, password, bearer);

  // Resume on mid-stream drops: each retry re-follows the original URL (a
  // fresh signed redirect) and continues from the bytes already on SD via a
  // Range request. Stop when an attempt makes no forward progress.
  DownloadError result;
  size_t lastDownloaded = 0;
  for (int attempt = 0;; ++attempt) {
    result = runGetSecure(url, sink, downgradeRedirectsToHttp);
    if (result != HTTP_ERROR) break;  // OK, ABORTED, FILE_ERROR: no retry
    if (attempt >= MAX_RESUME_ATTEMPTS) break;
    if (sink.downloaded == 0 || sink.total == 0 || sink.downloaded >= sink.total) break;
    if (sink.downloaded <= lastDownloaded) break;
    lastDownloaded = sink.downloaded;
    sink.resumeOffset = sink.downloaded;
    LOG_INF("HTTP", "Resuming download at %zu/%zu bytes (attempt %d)", sink.downloaded, sink.total, attempt + 1);
  }
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
