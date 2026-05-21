#include "HttpDownloader.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <base64.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// OtaUpdater workaround: the Arduino framework ships a stub esp_crt_bundle.h
// inside WiFiClientSecure that hides the real ESP-IDF symbol. Forward-declare
// the IDF entry point instead of including the header — see OtaUpdater.cpp.
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

namespace {
// ISRG Root X1 — Let's Encrypt's root CA. Pinned here because the Espressif
// crt_bundle's Subject-DN lookup can pick the wrong "ISRG Root X1" entry on
// cross-signed bundles and fail signature verification ("PK verify failed
// with error 0x4290" → MBEDTLS_ERR_X509_FATAL_ERROR -0x3000). We use this
// pin for raw.githubusercontent.com (Let's Encrypt-issued), and fall back to
// the default crt_bundle for all other hosts (DigiCert chain on
// github.com/api.github.com, etc.).
//
// Not-after: 2035-06-04. Update when Let's Encrypt rotates the root.
// Source: https://letsencrypt.org/certs/isrgrootx1.pem
constexpr const char ISRG_ROOT_X1_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

// mbedtls rejects certs whose notBefore lies in the future of the device
// clock, returning MBEDTLS_ERR_X509_CERT_VERIFY_FAILED (-0x2700). The
// ESP32-C3 has no battery-backed RTC, so cold-boot clocks default to 1970
// (or, if HalClock restored from NVS, a stale "last known" time that may
// still predate the cert's notBefore). Fix it once per process before the
// first https request by running SNTP — WiFi is already up by the time
// runGet() is called, so this is essentially free. Subsequent calls reuse
// whatever the first attempt produced.
constexpr time_t MIN_PLAUSIBLE_EPOCH = 1735689600;  // 2025-01-01 00:00:00 UTC
bool ensureClockForTls() {
  static bool attempted = false;
  if (attempted) return time(nullptr) >= MIN_PLAUSIBLE_EPOCH;
  attempted = true;

  if (HalClock::now() >= MIN_PLAUSIBLE_EPOCH && !HalClock::isApproximate()) {
    return true;
  }
  LOG_INF("HTTP", "Clock looks unset/stale (epoch %ld); running SNTP before TLS", static_cast<long>(time(nullptr)));
  char err[64] = {0};
  if (!HalClock::syncNtp(err, sizeof(err))) {
    LOG_ERR("HTTP", "SNTP sync failed: %s — TLS verification may fail until clock is set", err);
    return false;
  }
  LOG_INF("HTTP", "SNTP sync complete; epoch now %ld", static_cast<long>(time(nullptr)));
  return true;
}

// True if the URL's host is a *.githubusercontent.com host that's served by
// Let's Encrypt — needs the ISRG pin to dodge the crt_bundle Subject-collision
// bug. Adjust if more hosts hit the same issue.
bool needsLetsEncryptPin(const std::string& url) {
  // Strip scheme://, then everything from the first / onward.
  size_t schemeEnd = url.find("://");
  size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  const std::string host = url.substr(hostStart, hostEnd - hostStart);
  // raw.githubusercontent.com is the only one we've seen fail today. Match
  // the suffix so codeload.githubusercontent.com / etc. get the same fix.
  static constexpr const char* kSuffix = ".githubusercontent.com";
  const size_t suffixLen = strlen(kSuffix);
  return host.size() >= suffixLen && host.compare(host.size() - suffixLen, suffixLen, kSuffix) == 0;
}

// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces. Matches
// upstream PR #2075 (port of OtaUpdater's PR #2074 sizing).
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. esp_http_client's timeout_ms is uint32, so unlike
// Arduino HTTPClient's uint16 setTimeout it doesn't silently truncate. 60s
// gives slow servers room to send their first headers.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;

struct Sink {
  // Returns false to abort the transfer (e.g. SD write failure or user cancel).
  std::function<bool(const uint8_t*, size_t)> write;
  HttpDownloader::ProgressCallback progress;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Builds the esp_http_client_config_t for a given URL, picking the appropriate
// TLS root strategy (pinned ISRG vs default crt_bundle) based on the host.
void configureClient(const std::string& url, esp_http_client_config_t& config) {
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  if (needsLetsEncryptPin(url)) {
    config.cert_pem = ISRG_ROOT_X1_PEM;
    config.cert_len = sizeof(ISRG_ROOT_X1_PEM);
  } else {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
  config.keep_alive_enable = true;
}

void applyRequestHeaders(esp_http_client_handle_t client, const std::string& username, const std::string& password) {
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
}

// Performs the per-request work on an already-initialised client: open the
// connection (does the TLS handshake on first call; reuses the open TCP/TLS
// connection on subsequent calls per HTTP keep-alive), read headers, follow
// redirects, then stream the body. Used by both the standalone runGet and the
// Session-based path.
HttpDownloader::DownloadError performGet(esp_http_client_handle_t client, Sink& sink) {
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    int tlsCode = 0;
    int tlsFlags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags);
    LOG_ERR("HTTP", "open failed: %s (tls_code=-0x%04x, tls_flags=0x%08x)", esp_err_to_name(err), -tlsCode, tlsFlags);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    return HttpDownloader::HTTP_ERROR;
  }

  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  std::unique_ptr<char[]> buf(new (std::nothrow) char[READ_CHUNK]);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    return HttpDownloader::HTTP_ERROR;
  }

  bool aborted = false;
  while (true) {
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      aborted = true;
      break;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) {
      if (!sink.progress(sink.downloaded, sink.total)) {
        aborted = true;
        break;
      }
    }
  }

  if (aborted) {
    return HttpDownloader::ABORTED;
  }
  if (!esp_http_client_is_complete_data_received(client)) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// Runs once per http call (or once per session for reused sessions): logs
// heap stats and ensures the wall clock is set so TLS cert-date validation
// can succeed.
void logPreCallContext(const std::string& url) {
  LOG_DBG("HTTP", "Heap free: %u, largest block: %u", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  if (url.compare(0, 8, "https://") == 0) {
    ensureClockForTls();
  }
}

// Streams a GET body through sink.write in READ_CHUNK pieces. One-shot client:
// creates a fresh esp_http_client per call. See HttpDownloader::Session for
// the reusable variant that keeps the TLS handshake alive across files.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  logPreCallContext(url);

  esp_http_client_config_t config = {};
  configureClient(url, config);

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }
  applyRequestHeaders(client, username, password);
  const HttpDownloader::DownloadError result = performGet(client, sink);
  esp_http_client_cleanup(client);
  return result;
}
}  // namespace

// ---- Session implementation ----

struct HttpDownloader::Session::Impl {
  esp_http_client_handle_t client = nullptr;
  std::string host;  // scheme+authority of the first request; used to detect cross-host reuse

  ~Impl() {
    if (client) {
      esp_http_client_cleanup(client);
    }
  }
};

HttpDownloader::Session::Session() : impl_(std::make_unique<Impl>()) {}
HttpDownloader::Session::~Session() = default;

namespace {
// Extract "scheme://host[:port]" from a URL — used to detect when a Session
// is asked to reuse across hosts (esp_http_client supports it via set_url but
// it tears down and reopens the TLS connection, losing the heap win).
std::string schemeAuthority(const std::string& url) {
  size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return "";
  size_t pathStart = url.find('/', schemeEnd + 3);
  return url.substr(0, pathStart == std::string::npos ? url.size() : pathStart);
}

// Internal: initialise the session's underlying esp_http_client for the given
// URL. Used both for the first call and for reconnect-after-failure.
bool initSessionClient(HttpDownloader::Session::Impl* impl, const std::string& url) {
  esp_http_client_config_t config = {};
  configureClient(url, config);
  impl->client = esp_http_client_init(&config);
  if (!impl->client) {
    LOG_ERR("HTTP", "session client init failed");
    return false;
  }
  impl->host = schemeAuthority(url);
  return true;
}

HttpDownloader::DownloadError runGetOnSession(HttpDownloader::Session& session, const std::string& url,
                                              const std::string& username, const std::string& password, Sink& sink) {
  auto* impl = session.impl();

  if (impl->client == nullptr) {
    logPreCallContext(url);
    if (!initSessionClient(impl, url)) {
      return HttpDownloader::HTTP_ERROR;
    }
  } else {
    const std::string nextHost = schemeAuthority(url);
    if (nextHost != impl->host) {
      LOG_INF("HTTP", "Session URL host changed (%s -> %s); reopening", impl->host.c_str(), nextHost.c_str());
      impl->host = nextHost;
    }
    esp_err_t setUrlErr = esp_http_client_set_url(impl->client, url.c_str());
    if (setUrlErr != ESP_OK) {
      LOG_ERR("HTTP", "set_url failed: %s", esp_err_to_name(setUrlErr));
      return HttpDownloader::HTTP_ERROR;
    }
  }

  applyRequestHeaders(impl->client, username, password);
  HttpDownloader::DownloadError result = performGet(impl->client, sink);

  // If a reused client's open() failed (e.g. server closed idle keep-alive),
  // tear it down and try once more from a clean state. Critical for the
  // manifest→file flow where the user can sit on the family list for a while
  // before pressing confirm.
  if (result == HttpDownloader::HTTP_ERROR && sink.downloaded == 0) {
    LOG_INF("HTTP", "Session reuse failed; reinitialising client and retrying once");
    esp_http_client_cleanup(impl->client);
    impl->client = nullptr;
    if (!initSessionClient(impl, url)) {
      return HttpDownloader::HTTP_ERROR;
    }
    applyRequestHeaders(impl->client, username, password);
    result = performGet(impl->client, sink);
  }
  return result;
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

namespace {
// Common file-sink plumbing used by both downloadToFile overloads.
HttpDownloader::DownloadError finishFileDownload(HttpDownloader::DownloadError result, const std::string& destPath,
                                                 FsFile& file, size_t downloaded) {
  // Flush before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.flush();
  file.close();
  if (result != HttpDownloader::OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return HttpDownloader::OK;
}
}  // namespace

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const std::string& username,
                                                             const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGet(url, username, password, sink);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(Session& session, const std::string& url,
                                                             const std::string& destPath, ProgressCallback progress,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading (session): %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetOnSession(session, url, username, password, sink);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}
