#pragma once

// WitchReader SecureNet — minimal keep-alive HTTP/1.1 over SecureClient.
//
// Adapted from Free-Ink/freeink-sdk (MIT) SecureHttpClient and crosspoint-reader
// PR #2475, extended for WitchReader with: keep-alive session reuse, redirect
// following, streaming (non-buffering) body sink, HTTP basic auth, and plain-http
// passthrough (a plain WiFiClient transport when the scheme is http). It does NOT
// wrap Arduino HTTPClient (which binds a NetworkClient, incompatible with our
// SecureClient's owned-WiFiClient design).
//
// This is the single HTTP engine behind HttpDownloader and KOReaderSync now that
// the mbedtls/esp_http_client stack has been removed.
//
// OPT-IN: https requires -DFREEINK_NET_WOLFSSL=1. With the flag off, https
// connect() fails; plain-http still works over the WiFiClient transport.

#include <Arduino.h>
#include <WiFiClient.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "HttpRange.h"
#include "SecureClient.h"

namespace crosspoint {

class SecureHttpClient {
 public:
  // Body sink: called with each decoded body chunk; return false to abort.
  using BodySink = std::function<bool(const uint8_t* data, size_t len)>;
  // Progress: (downloaded, total); total==0 when Content-Length is unknown.
  // Return false to abort.
  using ProgressFn = std::function<bool(size_t downloaded, size_t total)>;

  SecureHttpClient() = default;
  ~SecureHttpClient() { close(); }

  SecureHttpClient(const SecureHttpClient&) = delete;
  SecureHttpClient& operator=(const SecureHttpClient&) = delete;

  // --- configuration (before request) ---
  void setCACert(const char* rootCA) { _rootCA = rootCA; }
  void setAllowInsecureFallback(bool allow) { _allowInsecureFallback = allow; }
  // See SecureClient::setAllowCertificateDateErrors().
  void setAllowCertificateDateErrors(bool allow) { _allowCertificateDateErrors = allow; }
  void setTimeout(uint32_t ms) { _timeoutMs = ms; }
  void setUserAgent(const std::string& ua) { _userAgent = ua; }
  // Basic auth and the addHeader() headers are the credentials of the server a request starts at
  // (KOSync's x-auth-user and x-auth-key ride in custom headers). They go only to that origin
  // (scheme, host, port): a redirect anywhere else is followed without them, and they stay off for
  // the rest of the request even if a later hop comes back. sameCredentialOrigin() has the rule.
  void setBasicAuth(const std::string& user, const std::string& pass) {
    _user = user;
    _pass = pass;
  }
  // Custom headers, sent until clearHeaders(). Credentials, as above.
  void addHeader(const std::string& name, const std::string& value) { _headers.push_back(name + ": " + value); }
  void clearHeaders() { _headers.clear(); }
  // The credentials belong to url's origin instead of the URL each request starts at, until
  // clearCredentialScope(). For a caller that sends later requests straight to where an earlier
  // one was redirected (a chunked download's later chunks): that target is not the server the
  // credentials were given for.
  void setCredentialScope(const std::string& url) {
    _credentialScopeUrl = url;
    _credentialScopePinned = true;
  }
  void clearCredentialScope() {
    _credentialScopeUrl.clear();
    _credentialScopePinned = false;
  }
  // Max redirect hops to follow (default 5; 0 disables following).
  void setMaxRedirects(int n) { _maxRedirects = n; }
  // Allow a redirect to step down from https to http. Off by default, because
  // the downgrade silently drops transport security; when refused, following
  // stops and the caller sees the 3xx status.
  void setAllowRedirectDowngrade(bool allow) { _allowRedirectDowngrade = allow; }
  // Asked with the absolute URL of each redirect before it is followed. False
  // stops there and the caller sees the 3xx status, as for a refused downgrade.
  // For requests made on someone else's behalf whose reach is fenced to an
  // allowlist: without it, an allowed host could send the request anywhere.
  using RedirectFilter = std::function<bool(const std::string& url)>;
  void setRedirectFilter(RedirectFilter filter) { _redirectFilter = std::move(filter); }
  // Ask for bytes first..last (inclusive) of the resource: a "Range: bytes=first-last" header on
  // every request, redirect hops included, until clearRange(). The response is a 206 with a
  // Content-Range (lastContentRange()), or a 200 from a server that ignores Range.
  void setRange(size_t first, size_t last) { _rangeHeader = http_range::rangeHeaderValue(first, last); }
  void clearRange() { _rangeHeader.clear(); }
  // Leave out the per-request trace line for requests that got a response (failures still log it),
  // for a transfer made of many small requests that logs one summary instead.
  void setQuietRequests(bool quiet) { _quietRequests = quiet; }
  // request() keeps at most this many body bytes (0: no limit) and reads the rest off the
  // connection without storing it, so the status and the start of the body still arrive.
  // bodyTruncated() says whether anything was dropped. For callers that expect a small reply but
  // may be pointed at a server that sends a large page instead.
  void setMaxBufferedBody(size_t bytes) { _maxBufferedBody = bytes; }

  // --- requests ---
  // Open the connection for url (or keep the matching kept-alive one) without sending anything, so
  // a caller can see the heap its requests will start from. False on a bad URL or a failed connect.
  bool open(const std::string& url);

  // Streaming GET: body is delivered to sink in chunks. Returns the final HTTP
  // status (after redirects), or a negative SecureHttpError on transport failure.
  int get(const std::string& url, const BodySink& sink, const ProgressFn& progress = nullptr);

  // Buffered request (GET/POST/PUT): full body accumulated into getBody(). Use
  // only for small responses (KOSync JSON). Returns HTTP status or negative error.
  int request(const char* method, const std::string& url, const std::string& body = "");
  int GET(const std::string& url) { return request("GET", url); }
  int POST(const std::string& url, const std::string& body) { return request("POST", url, body); }
  int PUT(const std::string& url, const std::string& body) { return request("PUT", url, body); }

  const std::string& getBody() const { return _body; }
  bool bodyTruncated() const { return _bodyTruncated; }
  // The current (inside a body sink) or last response: status, Content-Length (-1 when absent),
  // Content-Range, and the URL it came from after redirects. All set before the first body byte.
  int lastStatus() const { return _status; }
  long lastContentLength() const { return _contentLength; }
  bool lastHasContentRange() const { return _hasContentRange; }
  const http_range::ContentRange& lastContentRange() const { return _contentRange; }
  const std::string& lastUrl() const { return _lastUrl; }
  bool lastConnectionWasInsecure() const { return _lastInsecure; }
  // True if the last request failed because a TLS record buffer could not be allocated.
  bool lastReadOutOfMemory() const { return _secure.lastReadWasOutOfMemory(); }
  // Heap trough sampled across the last TLS handshake (see SecureClient). Only
  // meaningful for https requests; SIZE_MAX if no https handshake occurred.
  size_t lastHandshakeMinFree() const { return _secure.handshakeMinFree(); }
  size_t lastHandshakeMinLargest() const { return _secure.handshakeMinLargest(); }

  // Close the held-open keep-alive connection (if any).
  void close();

  // Negative return codes from get()/request() (distinct from HTTP status).
  enum SecureHttpError {
    ERR_BAD_URL = -1,
    ERR_CONNECT = -2,
    ERR_SEND = -3,
    ERR_TIMEOUT = -4,
    ERR_TOO_MANY_REDIRECTS = -5,
    ERR_ABORTED = -6,
    ERR_TRUNCATED = -7,
  };

 private:
  struct Url {
    std::string scheme;
    std::string host;
    std::string path;
    uint16_t port = 0;
    bool https() const { return scheme == "https"; }
  };
  static bool parseUrl(const std::string& in, Url& out);
  static std::string resolveRedirect(const Url& base, const std::string& location);
  // Whether hop is the origin the credentials belong to (scope), so that they may go there.
  static bool sameCredentialOrigin(const Url& scope, const Url& hop);
  // Whether hop, number index of a request, may carry the credentials. The first hop sets scope:
  // the pinned one, else its own origin. The caller stops asking once a hop says no, so no later
  // hop of the request gets them back.
  bool credentialsFollow(Url& scope, const Url& hop, int index) const;

  // Per-request trace for the half of a request that was never instrumented.
  //
  // SecureClient breaks the handshake down to the certificate; everything after it was a single
  // elapsed number, so a 5 s ERR_TIMEOUT could not say whether the request left the device,
  // whether bytes came back and we failed to read them, or whether the peer simply never
  // answered. Each field exists to separate one of those:
  //   connectMs          reaching a usable socket, including a handshake on a fresh connection
  //   sendMs/sendBytes   the request actually went out, and how long the socket took to take it
  //   firstByteMs        measured from the END of the send, so it is the peer's silence and not
  //                      our own handshake. 0 means NOTHING ever arrived
  //   readPolls          2 ms idle polls spent waiting; high with firstByteMs 0 is a dead wait
  //   headerBytes        bytes that did arrive, so a partial response is distinguishable
  // Paired with rssi and the modem-sleep mode, which is the other thing that was reasoned about
  // rather than measured.
  struct RequestTrace {
    uint32_t startMs = 0;
    uint32_t connectMs = 0;
    uint32_t sendDoneMs = 0;
    uint32_t sendMs = 0;
    size_t sendBytes = 0;
    uint32_t firstByteMs = 0;
    uint32_t readPolls = 0;
    size_t headerBytes = 0;
    uint8_t attempts = 0;
    bool reusedConnection = false;
  };
  RequestTrace _trace;
  void logRequestTrace(const char* method, const Url& u, int rc) const;

  // Response status line + header fields that drive body framing and reuse.
  struct ResponseMeta {
    int status = 0;
    long contentLength = -1;
    bool chunked = false;
    bool keepAlive = true;
    std::string location;
    bool hasContentRange = false;
    http_range::ContentRange contentRange;
  };
  // Publishes a response's metadata (lastStatus() and friends) before its body is read.
  void noteResponse(const ResponseMeta& meta, const std::string& url);

  // True when the kept-open connection matches (scheme,host,port) and still
  // looks alive. "Looks" is best-effort: the server may have closed it already,
  // which transact() handles with its one-shot retry.
  bool connectionMatches(const Url& u);
  // Ensure a live connection to (scheme,host,port); reuse the kept-open one when
  // it matches, else (re)open. Returns false on connect failure.
  bool ensureConnected(const Url& u);
  // Connect (or reuse), send the request, and read the response headers — with
  // one transparent retry when a reused keep-alive socket turns out to be dead.
  // Returns 0, or a negative SecureHttpError.
  int transact(const char* method, const Url& u, const uint8_t* body, size_t bodyLen, bool credentials,
               ResponseMeta& meta);
  // Send the request line + headers (+ body for POST/PUT), with the credentials only when told to.
  // Returns false on write failure.
  bool sendRequest(const char* method, const Url& u, const uint8_t* body, size_t bodyLen, bool credentials);
  // Read status line + headers into meta (status, body framing, Location, Content-Range).
  bool readHeaders(ResponseMeta& meta);
  // Stream the body per framing to sink (with progress). Returns 0 on success,
  // or a negative SecureHttpError.
  int readBody(const BodySink& sink, const ProgressFn& progress, long contentLength, bool chunked, bool keepAlive);

  bool readLine(std::string& line, uint32_t deadline);

  // Transport: one of these is active per connection depending on scheme.
  SecureClient _secure;
  WiFiClient _plain;
  Client* _client = nullptr;  // -> _secure or _plain while connected
  bool _connectedHttps = false;
  std::string _connHost;
  uint16_t _connPort = 0;

  // config
  const char* _rootCA = nullptr;
  bool _allowInsecureFallback = true;
  bool _allowCertificateDateErrors = false;
  uint32_t _timeoutMs = 15000;
  std::string _userAgent = "CrossPoint-ESP32";
  std::string _user;
  std::string _pass;
  std::vector<std::string> _headers;
  std::string _credentialScopeUrl;  // setCredentialScope(), parsed as each request starts
  bool _credentialScopePinned = false;
  std::string _rangeHeader;  // "bytes=a-b" while a Range is set, else empty
  bool _quietRequests = false;
  size_t _maxBufferedBody = 0;
  int _maxRedirects = 5;
  bool _allowRedirectDowngrade = false;
  RedirectFilter _redirectFilter;

  // per-request result
  std::string _body;
  bool _bodyTruncated = false;
  int _status = 0;
  long _contentLength = -1;
  bool _hasContentRange = false;
  http_range::ContentRange _contentRange;
  std::string _lastUrl;
  bool _lastInsecure = false;

  static constexpr size_t MAX_LINE = 4096;
  // Body read buffer. 2 KB (was 512) so we drain wolfSSL's decrypted TLS records
  // in far fewer read() calls — 512 throttled large downloads to ~30 KB/s and
  // let slow CDNs (Cloudflare) drop the connection mid-stream. Stack-allocated,
  // so kept modest. Matches the legacy esp_http_client READ_CHUNK.
  static constexpr size_t READ_CHUNK = 2048;
};

}  // namespace crosspoint
