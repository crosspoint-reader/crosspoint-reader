#include "SecureHttpClient.h"

#include <Logging.h>
#include <WiFi.h>
#include <base64.h>

#include <cstdlib>
#include <cstring>

namespace crosspoint {

namespace {
bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
std::string toLowerAscii(const std::string& s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return r;
}
}  // namespace

bool SecureHttpClient::parseUrl(const std::string& in, Url& out) {
  const size_t schemeEnd = in.find("://");
  if (schemeEnd == std::string::npos) return false;
  out.scheme = toLowerAscii(in.substr(0, schemeEnd));
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = in.find('/', hostStart);
  const std::string hostPort =
      pathStart == std::string::npos ? in.substr(hostStart) : in.substr(hostStart, pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : in.substr(pathStart);
  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos && hostPort.find(']') == std::string::npos) {
    out.host = hostPort.substr(0, portSep);
    out.port = static_cast<uint16_t>(atoi(hostPort.substr(portSep + 1).c_str()));
  } else {
    out.host = hostPort;
    out.port = out.scheme == "https" ? 443 : 80;
  }
  return !out.host.empty() && (out.scheme == "http" || out.scheme == "https");
}

std::string SecureHttpClient::resolveRedirect(const Url& base, const std::string& location) {
  if (location.find("://") != std::string::npos) return location;  // absolute
  if (!location.empty() && location[0] == '/') {                   // absolute path
    return base.scheme + "://" + base.host + ":" + std::to_string(base.port) + location;
  }
  // relative path: strip base path back to last '/'
  const size_t slash = base.path.rfind('/');
  const std::string dir = slash == std::string::npos ? "/" : base.path.substr(0, slash + 1);
  return base.scheme + "://" + base.host + ":" + std::to_string(base.port) + dir + location;
}

void SecureHttpClient::close() {
  if (_client) {
    _client->stop();
    _client = nullptr;
  }
  _connHost.clear();
  _connPort = 0;
  _connectedHttps = false;
}

bool SecureHttpClient::connectionMatches(const Url& u) {
  return _client && _client->connected() && _connectedHttps == u.https() && _connHost == u.host && _connPort == u.port;
}

bool SecureHttpClient::ensureConnected(const Url& u) {
  // Reuse a matching kept-open connection.
  if (connectionMatches(u)) return true;
  close();

  if (u.https()) {
    _secure.setCACert(_rootCA);
    // No trust store means the caller asked for an unverified connection. Say so
    // explicitly rather than letting connect() take the verified path and quietly
    // find nothing to verify against: this is what makes lastConnectWasInsecure()
    // — and every log line derived from it — tell the truth.
    _secure.setInsecure(_rootCA == nullptr);
    _secure.setAllowInsecureFallback(_allowInsecureFallback);
    _secure.setAllowCertificateDateErrors(_allowCertificateDateErrors);
    _secure.setTimeout(_timeoutMs / 1000);
    if (!_secure.connect(u.host.c_str(), u.port)) {
      LOG_ERR("HTTP", "https connect failed: %s:%u", u.host.c_str(), u.port);
      return false;
    }
    _lastInsecure = _secure.lastConnectWasInsecure();
    _client = &_secure;
    _connectedHttps = true;
  } else {
    _plain.setTimeout(_timeoutMs / 1000);
    if (!_plain.connect(u.host.c_str(), u.port)) {
      LOG_ERR("HTTP", "http connect failed: %s:%u", u.host.c_str(), u.port);
      return false;
    }
    _lastInsecure = false;
    _client = &_plain;
    _connectedHttps = false;
  }
  _connHost = u.host;
  _connPort = u.port;
  return true;
}

bool SecureHttpClient::open(const std::string& url) {
  Url u;
  if (!parseUrl(url, u)) return false;
  return ensureConnected(u);
}

void SecureHttpClient::noteResponse(const ResponseMeta& meta, const std::string& url) {
  _status = meta.status;
  _contentLength = meta.contentLength;
  _hasContentRange = meta.hasContentRange;
  _contentRange = meta.contentRange;
  _lastUrl = url;
}

bool SecureHttpClient::sendRequest(const char* method, const Url& u, const uint8_t* body, size_t bodyLen) {
  const uint16_t defPort = u.https() ? 443 : 80;
  const std::string hostHeader = (u.port == defPort) ? u.host : (u.host + ":" + std::to_string(u.port));

  std::string req = std::string(method) + " " + u.path + " HTTP/1.1\r\n";
  req += "Host: " + hostHeader + "\r\n";
  req += "User-Agent: " + _userAgent + "\r\n";
  // Keep-alive so a Session can reuse the handshake across files.
  req += "Connection: keep-alive\r\n";
  if (!_user.empty() && !_pass.empty()) {
    const std::string creds = _user + ":" + _pass;
    req += "Authorization: Basic " + std::string(base64::encode(creds.c_str()).c_str()) + "\r\n";
  }
  for (const std::string& h : _headers) req += h + "\r\n";
  if (!_rangeHeader.empty()) req += "Range: " + _rangeHeader + "\r\n";
  if (body && bodyLen) req += "Content-Length: " + std::to_string(bodyLen) + "\r\n";
  req += "\r\n";

  const uint32_t sendStartMs = millis();
  if (_client->write(reinterpret_cast<const uint8_t*>(req.data()), req.size()) != req.size()) return false;
  if (body && bodyLen) {
    if (_client->write(body, bodyLen) != bodyLen) return false;
  }
  _trace.sendDoneMs = millis();
  _trace.sendMs = _trace.sendDoneMs - sendStartMs;
  _trace.sendBytes = req.size() + bodyLen;
  return true;
}

// One line per request covering everything after the handshake. Read it as: did the request go
// out (sendBytes), did anything come back at all (ttfb, 0 = never), how long did we sit waiting
// (polls x 2 ms), and what did the link look like while we waited.
void SecureHttpClient::logRequestTrace(const char* method, const Url& u, int rc) const {
  LOG_DBG("HTTP", "%s %s: conn=%s/%lums send %lums/%uB ttfb=%lums polls=%lu hdr=%uB attempts=%u rssi=%d ps=%d -> rc=%d",
          method, u.host.c_str(), _trace.reusedConnection ? "reused" : "fresh",
          static_cast<unsigned long>(_trace.connectMs), static_cast<unsigned long>(_trace.sendMs),
          static_cast<unsigned>(_trace.sendBytes), static_cast<unsigned long>(_trace.firstByteMs),
          static_cast<unsigned long>(_trace.readPolls), static_cast<unsigned>(_trace.headerBytes), _trace.attempts,
          static_cast<int>(WiFi.RSSI()), static_cast<int>(WiFi.getSleep()), rc);
}

// Connect (or reuse), send, and read headers — with one transparent retry: a
// keep-alive server may close the socket between requests at any time, and the
// race surfaces as a failed write or a missing status line on a socket that
// looked connected. That failure belongs to the reused connection, not the
// request, so it earns exactly one attempt on a fresh connection. A request
// that failed on a fresh connection is a real error and is not retried.
int SecureHttpClient::transact(const char* method, const Url& u, const uint8_t* body, size_t bodyLen,
                               ResponseMeta& meta) {
  _trace = RequestTrace{};
  _trace.startMs = millis();
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool reusing = connectionMatches(u);
    _trace.reusedConnection = reusing;
    _trace.attempts = static_cast<uint8_t>(attempt + 1);
    const uint32_t connectStartMs = millis();
    if (!ensureConnected(u)) {
      logRequestTrace(method, u, ERR_CONNECT);
      return ERR_CONNECT;
    }
    _trace.connectMs = millis() - connectStartMs;
    if (!sendRequest(method, u, body, bodyLen)) {
      close();
      if (reusing && attempt == 0) continue;
      logRequestTrace(method, u, ERR_SEND);
      return ERR_SEND;
    }
    if (!readHeaders(meta)) {
      close();
      if (reusing && attempt == 0) continue;
      logRequestTrace(method, u, ERR_TIMEOUT);
      return ERR_TIMEOUT;
    }
    if (!_quietRequests) logRequestTrace(method, u, meta.status);
    return 0;
  }
  logRequestTrace(method, u, ERR_SEND);
  return ERR_SEND;  // unreachable: the second attempt always returns above
}

bool SecureHttpClient::readLine(std::string& line, uint32_t deadline) {
  line.clear();
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    while (_client->available() > 0) {
      const int ch = _client->read();
      if (ch < 0) break;
      // First decrypted byte of the response: the difference between "the peer never answered"
      // and "the answer arrived and something after this went wrong".
      if (_trace.firstByteMs == 0 && _trace.sendDoneMs != 0) {
        _trace.firstByteMs = millis() - _trace.sendDoneMs;
      }
      ++_trace.headerBytes;
      if (ch == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      if (line.size() >= MAX_LINE) return false;
      line += static_cast<char>(ch);
    }
    if (!_client->connected() && _client->available() == 0) return false;
    ++_trace.readPolls;
    delay(2);
  }
  return false;
}

bool SecureHttpClient::readHeaders(ResponseMeta& meta) {
  meta = ResponseMeta{};  // HTTP/1.1 defaults: keep-alive, no length, not chunked
  int& status = meta.status;
  long& contentLength = meta.contentLength;
  bool& chunked = meta.chunked;
  bool& keepAlive = meta.keepAlive;
  std::string& location = meta.location;

  const uint32_t deadline = millis() + _timeoutMs;
  std::string line;
  if (!readLine(line, deadline)) return false;
  // "HTTP/1.1 200 OK" -> code at offset 9
  status = line.size() >= 12 ? atoi(line.c_str() + 9) : 0;
  if (status == 0) return false;
  // HTTP/1.0 peers default to connection-per-request; only an explicit
  // Connection: keep-alive header (parsed below) overrides that.
  if (line.compare(0, 9, "HTTP/1.0 ") == 0) keepAlive = false;

  while (readLine(line, deadline)) {
    if (line.empty()) return true;  // end of headers
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = toLowerAscii(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (name == "content-length") {
      contentLength = strtol(value.c_str(), nullptr, 10);
    } else if (name == "transfer-encoding") {
      if (toLowerAscii(value).find("chunked") != std::string::npos) chunked = true;
    } else if (name == "connection") {
      if (toLowerAscii(value).find("close") != std::string::npos) keepAlive = false;
    } else if (name == "location") {
      location = value;
    } else if (name == "content-range") {
      meta.hasContentRange = http_range::parseContentRange(value.c_str(), meta.contentRange);
    }
  }
  return false;  // ran out before blank line
}

// Truncation-safe body reader. Completion is keyed on framing, NEVER on read()==0
// (which means WANT_READ / no data yet for the wolfSSL transport).
int SecureHttpClient::readBody(const BodySink& sink, const ProgressFn& progress, long contentLength, bool chunked,
                               bool keepAlive) {
  uint8_t buf[READ_CHUNK];
  const size_t total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;
  size_t downloaded = 0;
  uint32_t idleDeadline = millis() + _timeoutMs;

  auto emit = [&](const uint8_t* d, size_t n) -> bool {
    if (sink && n && !sink(d, n)) return false;
    downloaded += n;
    if (progress && !progress(downloaded, total)) return false;
    return true;
  };

  if (chunked) {
    std::string line;
    for (;;) {
      if (!readLine(line, millis() + _timeoutMs)) return ERR_TRUNCATED;
      const size_t semi = line.find(';');
      const std::string sizeText = semi == std::string::npos ? line : line.substr(0, semi);
      char* parseEnd = nullptr;
      const unsigned long sz = strtoul(sizeText.c_str(), &parseEnd, 16);
      // A garbage size line must fail loudly: signed strtol's 0-on-garbage was
      // previously taken for the final chunk, reporting a truncated body as
      // success (and a negative value became a huge size_t that hung until
      // the timeout).
      if (parseEnd == sizeText.c_str()) return ERR_TRUNCATED;
      if (sz == 0) {  // last chunk; drain trailers
        while (readLine(line, millis() + _timeoutMs) && !line.empty()) {
        }
        return 0;
      }
      size_t remaining = static_cast<size_t>(sz);
      while (remaining > 0) {
        const int n = _client->read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (n < 0) return ERR_TRUNCATED;
        if (n == 0) {  // WANT_READ: no full TLS record decrypted yet
          if (!_client->connected() && _client->available() == 0) return ERR_TRUNCATED;
          if (static_cast<int32_t>(millis() - idleDeadline) >= 0) return ERR_TIMEOUT;
          // Only sleep when the transport is truly empty; if TCP bytes are
          // buffered, wolfSSL just needs another read to assemble the record.
          if (_client->available() == 0) delay(2);
          continue;
        }
        idleDeadline = millis() + _timeoutMs;
        if (!emit(buf, static_cast<size_t>(n))) return ERR_ABORTED;
        remaining -= static_cast<size_t>(n);
      }
      readLine(line, millis() + _timeoutMs);  // consume the chunk's trailing CRLF
    }
  }

  if (contentLength >= 0) {  // fixed length
    size_t remaining = static_cast<size_t>(contentLength);
    while (remaining > 0) {
      const int n = _client->read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
      if (n < 0) return ERR_TRUNCATED;
      if (n == 0) {
        if (!_client->connected() && _client->available() == 0) return ERR_TRUNCATED;
        if (static_cast<int32_t>(millis() - idleDeadline) >= 0) return ERR_TIMEOUT;
        if (_client->available() == 0) delay(2);
        continue;
      }
      idleDeadline = millis() + _timeoutMs;
      if (!emit(buf, static_cast<size_t>(n))) return ERR_ABORTED;
      remaining -= static_cast<size_t>(n);
    }
    return 0;
  }

  // No length, no chunked: read until the peer closes (Connection: close).
  for (;;) {
    const int n = _client->read(buf, sizeof(buf));
    // Over TLS a close reads as 0 (SecureClient::read), so -1 is a real failure, such as a record
    // that could not be allocated, and must not pass for the end of the body. A plain WiFiClient
    // reports the peer's close as -1, which is the end here.
    if (n < 0) return _client == &_secure ? ERR_TRUNCATED : 0;
    if (n == 0) {
      if (!_client->connected() && _client->available() == 0) return 0;  // clean end
      if (static_cast<int32_t>(millis() - idleDeadline) >= 0) return ERR_TIMEOUT;
      if (_client->available() == 0) delay(2);
      continue;
    }
    idleDeadline = millis() + _timeoutMs;
    if (!emit(buf, static_cast<size_t>(n))) return ERR_ABORTED;
  }
}

int SecureHttpClient::get(const std::string& url, const BodySink& sink, const ProgressFn& progress) {
  std::string current = url;
  for (int hop = 0; hop <= _maxRedirects; ++hop) {
    Url u;
    if (!parseUrl(current, u)) return ERR_BAD_URL;
    ResponseMeta meta;
    const int trc = transact("GET", u, nullptr, 0, meta);
    if (trc < 0) return trc;
    noteResponse(meta, current);

    if (isRedirect(meta.status) && !meta.location.empty()) {
      // Drain the redirect's (usually empty) body to keep the socket usable for
      // the next hop; a failed drain leaves undrained bytes, so close instead.
      if (readBody(nullptr, nullptr, meta.contentLength, meta.chunked, meta.keepAlive) < 0 || !meta.keepAlive) close();
      current = resolveRedirect(u, meta.location);
      // Refuse to drop TLS silently: stop following a https -> http downgrade
      // and surface the 3xx to the caller (setAllowRedirectDowngrade opts in).
      Url next;
      if (u.https() && !_allowRedirectDowngrade && parseUrl(current, next) && !next.https()) return meta.status;
      if (_redirectFilter && !_redirectFilter(current)) return meta.status;
      if (hop == _maxRedirects) return ERR_TOO_MANY_REDIRECTS;
      continue;
    }

    const int rc = readBody(sink, progress, meta.contentLength, meta.chunked, meta.keepAlive);
    if (rc < 0) {
      // Undrained body bytes would poison the kept-alive socket: the next
      // request would parse the leftovers as its status line.
      close();
      return rc;
    }
    // A close-delimited body (no framing) ends WITH the connection; never keep it.
    if (!meta.keepAlive || (meta.contentLength < 0 && !meta.chunked)) close();
    return meta.status;
  }
  return ERR_TOO_MANY_REDIRECTS;
}

int SecureHttpClient::request(const char* method, const std::string& url, const std::string& body) {
  _body.clear();
  _bodyTruncated = false;
  auto sink = [this](const uint8_t* d, size_t n) {
    if (_maxBufferedBody > 0 && _body.size() + n > _maxBufferedBody) {
      n = _maxBufferedBody - _body.size();
      _bodyTruncated = true;
    }
    _body.append(reinterpret_cast<const char*>(d), n);
    return true;
  };

  std::string current = url;
  std::string activeMethod = method;
  std::string activeBody = body;
  for (int hop = 0; hop <= _maxRedirects; ++hop) {
    Url u;
    if (!parseUrl(current, u)) return ERR_BAD_URL;
    ResponseMeta meta;
    const bool hasBody = !activeBody.empty();
    const int trc =
        transact(activeMethod.c_str(), u, hasBody ? reinterpret_cast<const uint8_t*>(activeBody.data()) : nullptr,
                 activeBody.size(), meta);
    if (trc < 0) return trc;
    noteResponse(meta, current);

    if (isRedirect(meta.status) && !meta.location.empty()) {
      // See get(): drain to keep the socket usable, close on a failed drain.
      if (readBody(nullptr, nullptr, meta.contentLength, meta.chunked, meta.keepAlive) < 0 || !meta.keepAlive) close();
      current = resolveRedirect(u, meta.location);
      // Refuse to drop TLS silently: stop following a https -> http downgrade
      // and surface the 3xx to the caller (setAllowRedirectDowngrade opts in).
      Url next;
      if (u.https() && !_allowRedirectDowngrade && parseUrl(current, next) && !next.https()) return meta.status;
      if (_redirectFilter && !_redirectFilter(current)) return meta.status;
      // 303 always -> GET; 301/302 commonly downgrade POST->GET too. 307/308
      // preserve method + body. When downgrading to GET, drop the request body.
      if (meta.status == 303 || meta.status == 301 || meta.status == 302) {
        activeMethod = "GET";
        activeBody.clear();
      }
      if (hop == _maxRedirects) return ERR_TOO_MANY_REDIRECTS;
      continue;
    }

    const int rc = readBody(sink, nullptr, meta.contentLength, meta.chunked, meta.keepAlive);
    if (rc < 0) {
      // Undrained body bytes would poison the kept-alive socket: the next
      // request would parse the leftovers as its status line.
      close();
      return rc;
    }
    // A close-delimited body (no framing) ends WITH the connection; never keep it.
    if (!meta.keepAlive || (meta.contentLength < 0 && !meta.chunked)) close();
    return meta.status;
  }
  return ERR_TOO_MANY_REDIRECTS;
}

}  // namespace crosspoint
