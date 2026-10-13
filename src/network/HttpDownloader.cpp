#include "HttpDownloader.h"

#include <Arduino.h>
#include <CrossPointRoots.h>
#include <HalClock.h>
#include <Logging.h>
#include <RangeDownload.h>
#include <SecureHttpClient.h>
#include <base64.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "CrossPointSettings.h"

// All HTTPS runs over the wolfSSL-backed SecureNet stack (verified against the
// curated CrossPointRoots); plain http uses SecureNet's WiFiClient passthrough.
// The former mbedtls/esp_http_client path (with its per-host cert pins and
// crt_bundle workarounds) has been removed — TLS 1.3 support and lower heap use
// were the reasons for the switch. See lib/SecureNet.

namespace {

std::string extractHostFromUrl(const std::string& url) {
  size_t schemeEnd = url.find("://");
  size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  return url.substr(hostStart, hostEnd - hostStart);
}

// Clock guard for TLS. The plausibility window and the SNTP retry policy now live in
// HalClock (isPlausibleForTls / ensureUsableForTls) so every TLS entry point shares one rule —
// this used to be a private copy here, which is why the KOReader paths never got it. Returns
// whether full certificate date validation is possible; false means the caller should tolerate
// date errors (and only date errors) for this request.
bool ensureClockForTls() { return HalClock::ensureUsableForTls(SETTINGS.ntpServer); }

// Per-request timeout handed to SecureHttpClient::setTimeout(). 60s gives slow
// servers room to send their first headers; SecureHttpClient reuses it as the
// idle deadline for each body read. The response body streams in
// SecureHttpClient's own READ_CHUNK-sized pieces.
constexpr int HTTP_TIMEOUT_MS = 60000;

// The sink, the chunked-download loop and its session state live in lib/SecureNet (RangeDownload.h),
// where they are host-tested.
using Sink = crosspoint::DownloadSink;

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Runs once per http call (or once per session for reused sessions): logs
// heap stats and ensures the wall clock is set so TLS cert-date validation
// can succeed. Shared by both TLS backends. Returns false when https was requested and the
// clock could not be established — the caller then permits date errors alone.
bool logPreCallContext(const std::string& url) {
  LOG_DBG("HTTP", "Heap free: %u, largest block: %u", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  if (url.compare(0, 8, "https://") == 0) {
    return ensureClockForTls();
  }
  return true;
}

// Does this transfer verify the peer? Resolves the caller's policy against the
// user's "skip HTTPS validation" setting — the escape hatch for self-hosted
// servers with a private CA or a self-signed certificate, which the device
// otherwise cannot reach at all. Strict ignores it by design.
bool verificationRequested(HttpDownloader::TlsPolicy tls) {
  switch (tls) {
    case HttpDownloader::TlsPolicy::Strict:
      return true;
    case HttpDownloader::TlsPolicy::Verified:
      return !SETTINGS.skipHttpsValidation;
    case HttpDownloader::TlsPolicy::Unverified:
      return false;
  }
  return true;
}

// Modem sleep parks the radio between DTIM beacons, which costs packets on a transfer long
// enough to span them -- so a small OPDS feed usually gets away with it and a large category
// or a multi-MB book consistently does not, surfacing as a stall or a short read rather than
// a clean error. OtaUpdater has disabled power-save around firmware downloads for exactly
// this reason since it was written; OPDS feeds and book fetches, which run just as long,
// never did.
// Ported from crosspoint-reader PR #3252 (Foulad / @sfoulad).
//
// Scoped to the request, not the session: a Session's keep-alive can sit idle between files
// while the user picks the next one, and holding the radio awake through that would spend
// battery for nothing.
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() {
    const esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to disable WiFi power-save: %d", err);
  }
  ~WifiPowerSaveGuard() {
    const esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to restore WiFi power-save: %d", err);
  }
};

size_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); }

// One-shot streaming GET over SecureNet (wolfSSL). Fills the Sink and emits
// "Phase start"/"Phase open_ok"/"Phase done" heap telemetry. The TlsPolicy
// decides whether the curated roots are loaded at all and what happens when the
// peer fails to verify.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink, HttpDownloader::TlsPolicy tls,
                                           const HttpDownloader::RedirectFilter& redirectAllowed) {
  const WifiPowerSaveGuard psGuard;
  const bool clockReady = logPreCallContext(url);
  const unsigned long startMs = millis();
  LOG_DBG("HTTP", "Phase start @%lums heap=%u largest=%u", millis() - startMs, esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  crosspoint::SecureHttpClient http;
  // No roots => SecureClient sets WOLFSSL_VERIFY_NONE and skips the hostname
  // check: one handshake, no chain work.
  http.setCACert(verificationRequested(tls) ? CROSSPOINT_ROOTS_PEM : nullptr);
  // Never retry unverified behind the caller's back; see TlsPolicy.
  http.setAllowInsecureFallback(false);
  http.setAllowCertificateDateErrors(!clockReady);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    http.setBasicAuth(username, password);
  }
  if (redirectAllowed) http.setRedirectFilter(redirectAllowed);

  bool openLogged = false;
  const auto write = sink.write;
  sink.write = [&openLogged, startMs, write](const uint8_t* data, size_t len) -> bool {
    if (!openLogged) {
      openLogged = true;
      LOG_DBG("HTTP", "Phase open_ok @%lums heap=%u largest=%u", millis() - startMs, esp_get_free_heap_size(),
              heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    return write(data, len);
  };

  // A one-shot download is its own session: whatever it learns ends with it.
  crosspoint::ChunkSession chunking;
  const int rc = crosspoint::downloadToSink(http, url, sink, chunking, &largestFreeBlock);
  // Log the handshake heap trough on EVERY path (incl. early abort via
  // treatAbortAsSuccess) so the TLS-specific low-water is always captured,
  // distinct from the all-time ESP.getMinFreeHeap() figure.
  LOG_DBG("HTTP", "Phase done @%lums rc=%d downloaded=%zu (insecure=%d) handshakeMinFree=%u handshakeMinLargest=%u",
          millis() - startMs, rc, sink.downloaded, static_cast<int>(http.lastConnectionWasInsecure()),
          static_cast<unsigned>(http.lastHandshakeMinFree()), static_cast<unsigned>(http.lastHandshakeMinLargest()));
  if (rc == crosspoint::SecureHttpClient::ERR_ABORTED) {
    return HttpDownloader::ABORTED;
  }
  if (rc == crosspoint::ERR_REWIND) return HttpDownloader::FILE_ERROR;
  if (rc < 0) {
    LOG_ERR("HTTP", "SecureNet GET failed: rc=%d url=%s", rc, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (rc != 200) {
    LOG_ERR("HTTP", "SecureNet unexpected status: %d", rc);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// Single funnel for every fetchUrl/downloadToFile overload. The policy defaults
// to the historic browsing posture; callers that carry credentials, install
// files, or fetch public data pass their own.
HttpDownloader::DownloadError runGetDispatch(const std::string& url, const std::string& username,
                                             const std::string& password, Sink& sink, HttpDownloader::TlsPolicy tls,
                                             const HttpDownloader::RedirectFilter& redirectAllowed = {}) {
  return runGetSecure(url, username, password, sink, tls, redirectAllowed);
}
}  // namespace

// ---- Session implementation ----

struct HttpDownloader::Session::Impl {
  // SecureNet keeps its own keep-alive connection alive internally (reuses the
  // open SecureClient when host/port match), so the Session just owns one
  // persistent SecureHttpClient across downloadToFile(session, ...) calls.
  std::unique_ptr<crosspoint::SecureHttpClient> http;
  bool preCallLogged = false;
  // What this session's files learn about chunked downloads (RangeDownload.h): a chunk size that ran
  // out of memory stays out of reach, so later files start at a size that worked.
  crosspoint::ChunkSession chunking;
};

HttpDownloader::Session::Session() : impl_(std::make_unique<Impl>()) {}
HttpDownloader::Session::~Session() = default;

namespace {

// SecureNet session GET: reuse one persistent SecureHttpClient. Its internal
// keep-alive reuses the open TLS connection when the host/port match, so
// back-to-back files on the same host share a single handshake (the Session
// heap win). Cross-host requests transparently reopen inside SecureHttpClient.
HttpDownloader::DownloadError runGetSecureOnSession(HttpDownloader::Session& session, const std::string& url,
                                                    const std::string& username, const std::string& password,
                                                    Sink& sink, HttpDownloader::TlsPolicy tls) {
  const WifiPowerSaveGuard psGuard;
  auto* impl = session.impl();
  // Evaluated on every call, not just when the session is created: a session opened before the
  // clock was set must not keep that verdict for the rest of its life (nor the reverse).
  const bool clockReady = logPreCallContext(url);
  if (!impl->http) {
    impl->http = std::make_unique<crosspoint::SecureHttpClient>();
    impl->http->setTimeout(HTTP_TIMEOUT_MS);
    impl->http->setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  }
  // Re-applied per call, like the clock verdict below: a session outlives a
  // single request and must not carry a previous caller's policy.
  impl->http->setCACert(verificationRequested(tls) ? CROSSPOINT_ROOTS_PEM : nullptr);
  impl->http->setAllowInsecureFallback(false);
  impl->http->setAllowCertificateDateErrors(!clockReady);
  impl->http->clearHeaders();
  if (!username.empty() && !password.empty()) {
    impl->http->setBasicAuth(username, password);
  }

  const int rc = crosspoint::downloadToSink(*impl->http, url, sink, impl->chunking, &largestFreeBlock);
  if (rc == crosspoint::SecureHttpClient::ERR_ABORTED) return HttpDownloader::ABORTED;
  if (rc == crosspoint::ERR_REWIND) return HttpDownloader::FILE_ERROR;
  if (rc != 200) {
    LOG_ERR("HTTP", "SecureNet session GET failed: rc=%d url=%s", rc, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runGetOnSession(HttpDownloader::Session& session, const std::string& url,
                                              const std::string& username, const std::string& password, Sink& sink,
                                              HttpDownloader::TlsPolicy tls) {
  return runGetSecureOnSession(session, url, username, password, sink, tls);
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password, TlsPolicy tls) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetDispatch(url, username, password, sink, tls) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  // Preserve historic semantics: callback abort => failure.
  return fetchUrl(url, onData, false, username, password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                              const std::string& username, const std::string& password,
                              const RedirectFilter& redirectAllowed) {
  LOG_DBG("HTTP", "Fetching (stream): %s", url.c_str());
  if (!onData) {
    return false;
  }
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  const DownloadError result = runGetDispatch(url, username, password, sink, TlsPolicy::Verified, redirectAllowed);
  if (result == OK) {
    return true;
  }
  // Optional mode for parsers that intentionally stop early once they have
  // extracted all required fields from the stream.
  return treatAbortAsSuccess && result == ABORTED;
}

bool HttpDownloader::fetchUrlVerified(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                                      const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Fetching (stream, verify-only): %s", url.c_str());
  if (!onData) {
    return false;
  }
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  // Strict: OTA never honours the skip-validation setting.
  const DownloadError result = runGetDispatch(url, username, password, sink, TlsPolicy::Strict);
  if (result == OK) {
    return true;
  }
  return treatAbortAsSuccess && result == ABORTED;
}

HttpDownloader::DownloadError HttpDownloader::fetchVerifiedRestartable(const std::string& url,
                                                                       const DataCallback& onData,
                                                                       const std::function<bool()>& restart) {
  LOG_DBG("HTTP", "Fetching (restartable, verify-only): %s", url.c_str());
  if (!onData || !restart) return HTTP_ERROR;
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  sink.rewind = restart;  // what makes the transfer eligible for Range chunks
  return runGetDispatch(url, "", "", sink, TlsPolicy::Strict);
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, TlsPolicy tls) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetDispatch(url, username, password, sink, tls) == OK;
}

namespace {
// A download is written beside its destination and takes its place only once complete, so a
// failed or cancelled download leaves the file that was there (a book downloaded again from a
// catalog, say) instead of deleting it first.
// From CrossInk commit 1d954ae34 ("preserve existing books during OPDS downloads", Julia Nguyen),
// which stages OPDS downloads as .part files; doing it here for every download is ours.
std::string partPathFor(const std::string& destPath) { return destPath + ".part"; }

// Common file-sink plumbing used by both downloadToFile overloads.
HttpDownloader::DownloadError finishFileDownload(HttpDownloader::DownloadError result, const std::string& destPath,
                                                 FsFile& file, size_t downloaded) {
  const std::string partPath = partPathFor(destPath);
  // Flush before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.flush();
  const bool closed = file.close();
  if (result != HttpDownloader::OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(partPath.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (!closed) {
    LOG_ERR("HTTP", "Failed to finish writing %s", partPath.c_str());
    Storage.remove(partPath.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move %s into place", partPath.c_str());
    Storage.remove(partPath.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return HttpDownloader::OK;
}
}  // namespace

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const std::string& username,
                                                             const std::string& password, TlsPolicy tls,
                                                             const RedirectFilter& redirectAllowed) {
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  const std::string partPath = partPathFor(destPath);
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", partPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the file starts again from byte 0.
  sink.rewind = [&file, &partPath]() {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };

  const DownloadError result = runGetDispatch(url, username, password, sink, tls, redirectAllowed);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(Session& session, const std::string& url,
                                                             const std::string& destPath, ProgressCallback progress,
                                                             const std::string& username, const std::string& password,
                                                             TlsPolicy tls) {
  LOG_DBG("HTTP", "Downloading (session): %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  const std::string partPath = partPathFor(destPath);
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", partPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the file starts again from byte 0.
  sink.rewind = [&file, &partPath]() {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };

  const DownloadError result = runGetOnSession(session, url, username, password, sink, tls);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}
