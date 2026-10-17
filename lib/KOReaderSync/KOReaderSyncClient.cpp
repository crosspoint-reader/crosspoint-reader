#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointRoots.h>
#include <HalClock.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include "KOReaderCredentialStore.h"
#include "KOReaderProgressReply.h"

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastEspError = 0;
unsigned KOReaderSyncClient::lastHeapAtFailure = 0;
unsigned KOReaderSyncClient::lastContigHeapAtFailure = 0;
const char* KOReaderSyncClient::lastOperation = "";

namespace {
// Retryable transport-failure sentinels for performKoRequest()'s esp_err_t-shaped
// return. Previously came from esp_http_client.h (KO_ERR_CONNECT/EAGAIN);
// that header is gone with the mbedtls stack, so define local values in the
// ESP_ERR_HTTP base range. Only produced/consumed within this file.
constexpr esp_err_t KO_ERR_CONNECT = static_cast<esp_err_t>(0x7001);  // connect/transport failure (retryable)
constexpr esp_err_t KO_ERR_EAGAIN = static_cast<esp_err_t>(0x7002);   // timeout/truncated (retryable)
// The most of a sync reply kept in memory; see performKoRequest().
constexpr size_t KO_MAX_RESPONSE_BYTES = 16 * 1024;

// Mirrors the app's "skip HTTPS validation" setting; see setSkipTlsValidation().
bool g_skipTlsValidation = false;

bool g_keepSessionOpen = false;
// Persistent wolfSSL HTTP client for a KOSync session. Its internal keep-alive
// reuses the TLS connection across GET-progress -> PUT-update on the same host,
// amortizing the handshake (the former g_sessionClient heap win). Null unless a
// persistent session is active.
std::unique_ptr<crosspoint::SecureHttpClient> g_sessionHttp;

// Static buffer for the detail string returned by lastFailureDetail() — sized to fit
// the longest expected message including esp_err name (~32 chars), opcode (~10), heap
// numbers, and HTTP status. Single-threaded sync flow makes static safe.
char g_failureDetailBuf[160] = {0};
char g_lastResponsePreview[160] = {0};

// Set when a request was rejected as INVALID_RESPONSE. lastFailureDetail() cannot infer this
// from the status code any more: a captive portal's HTML is now rejected under 404 as well as
// under 2xx, and 404 otherwise means a legitimate "no stored progress".
bool g_lastInvalidBody = false;

std::string previewBody(const char* body, const size_t maxLen = 120) {
  if (!body || !*body) {
    return "<empty>";
  }

  std::string preview;
  preview.reserve(maxLen);
  for (const char* p = body; *p && preview.size() < maxLen; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '\r' || c == '\n' || c == '\t') {
      preview.push_back(' ');
    } else if (std::isprint(c)) {
      preview.push_back(static_cast<char>(c));
    } else {
      preview.push_back('?');
    }
  }

  if (strlen(body) > preview.size()) {
    preview += "...";
  }
  return preview;
}

void rememberResponsePreview(const char* body) {
  const std::string preview = previewBody(body, sizeof(g_lastResponsePreview) - 1);
  strncpy(g_lastResponsePreview, preview.c_str(), sizeof(g_lastResponsePreview) - 1);
  g_lastResponsePreview[sizeof(g_lastResponsePreview) - 1] = '\0';
}

// Log exactly what the server answered with. KOSync-compatible servers disagree about how
// they say "nothing stored" — bodiless, "{}", "[]", "null", a lone newline, a BOM — and the
// printable preview cannot tell those apart, so short bodies are dumped as hex as well.
// The elapsed time covers connect + handshake + request + body read, and is paired with
// whether a handshake actually ran, because that is what separates "the server is slow" from
// "we paid for a new TLS session". INFO level: one line per request, and it is the first
// thing a sync bug report needs.
void logResponseBody(const char* method, const std::string& url, int status, const std::string& body,
                     uint32_t elapsedMs, bool didHandshake) {
  std::string hex;
  const size_t hexLen = std::min<size_t>(body.size(), 24);
  hex.reserve(hexLen * 3);
  for (size_t i = 0; i < hexLen; ++i) {
    char byteText[4];
    snprintf(byteText, sizeof(byteText), "%02X ", static_cast<unsigned char>(body[i]));
    hex += byteText;
  }
  if (body.size() > hexLen) hex += "...";
  if (hex.empty()) hex = "-";

  LOG_INF("KOSync", "Response %s %s -> HTTP %d in %lu ms (%s) | len=%u | body=%s | hex=%s", method, url.c_str(), status,
          static_cast<unsigned long>(elapsedMs), didHandshake ? "fresh handshake" : "session reused",
          static_cast<unsigned>(body.size()), previewBody(body.c_str()).c_str(), hex.c_str());
}

// Reset the static diagnostic state at the start of each request and capture pre-flight
// heap so failure reporting always reflects what was available when the request started.
void beginRequest(const char* operation) {
  KOReaderSyncClient::lastOperation = operation;
  KOReaderSyncClient::lastEspError = 0;
  KOReaderSyncClient::lastHttpCode = 0;
  KOReaderSyncClient::lastHeapAtFailure = ESP.getFreeHeap();
  KOReaderSyncClient::lastContigHeapAtFailure = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  g_lastResponsePreview[0] = '\0';
  g_lastInvalidBody = false;
}

// Skip a leading UTF-8 BOM (EF BB BF) and ASCII whitespace, returning a pointer
// to the first content character.  Used by response-body checks that verify the
// payload opens a JSON document rather than '<' (HTML captive-portal page).
const char* skipBomAndWhitespace(const char* p) {
  // UTF-8 BOM
  if (static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
      static_cast<unsigned char>(p[2]) == 0xBF) {
    p += 3;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  return p;
}

// A body carrying no content: absent, or nothing but a BOM and whitespace. Servers that
// answer "nothing stored" with a bare 200 sometimes still emit a newline, which must read
// the same as a truly empty body rather than as a JSON parse error.
bool bodyIsEffectivelyEmpty(const std::string& body) {
  return body.empty() || *skipBomAndWhitespace(body.c_str()) == '\0';
}

// True when a body is present but cannot be the JSON the KOSync API speaks. That did not come
// from the API — it is the signature of a captive portal or reverse proxy answering with its
// own HTML — so the accompanying status code cannot be trusted to mean what it says.
//
// Demanding a JSON *object* here was too strict: KOSync-compatible servers signal "nothing
// stored for this document" with whatever their framework serializes an absent record as —
// kosync.rustysoft.de answers HTTP 200 with the empty string "" (hex 22 22), others use "[]"
// or a bare null — and every one of those was reported to the user as a captive portal.
// Accept any JSON root an "empty record" can serialize to and let the endpoint decide what it
// means; only a body that cannot be JSON at all ('<' for HTML, prose from a proxy) is rejected
// here. Numbers and true/false stay rejected: no server encodes an absent record as either,
// and letting them through would blunt the portal/proxy heuristic this check exists for.
//
// An empty body is deliberately NOT a portal signature: several endpoints legitimately
// answer bodiless (204/205), and portals always serve a page. Callers that additionally
// require a body decide that for themselves.
bool bodyIsNotJson(const std::string& body) {
  if (bodyIsEffectivelyEmpty(body)) return false;
  const char* p = skipBomAndWhitespace(body.c_str());
  return *p != '{' && *p != '[' && *p != '"' && strncmp(p, "null", 4) != 0;
}

// Screen a response body before its status code is interpreted. Returns true when the caller
// should give up with INVALID_RESPONSE, having recorded the diagnostic state for
// lastFailureDetail().
bool shouldRejectBody(int httpCode, const std::string& body) {
  if (!bodyIsNotJson(body)) return false;
  g_lastInvalidBody = true;
  rememberResponsePreview(body.c_str());
  LOG_ERR("KOSync", "HTTP %d body is not JSON — refusing to interpret it: %s", httpCode, g_lastResponsePreview);
  return true;
}

// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// Keep strict thresholding here. A small tolerance caused repeated handshake
// attempts in borderline-fragmented states that still failed in the TLS layer.
constexpr unsigned TLS_CONTIG_HEAP_TOLERANCE = 0;

// Captures radio/link state around failed connects.
// Why: many field failures look like TLS errors but are actually weak WiFi.
void logWifiSnapshot(const char* stage) {
  const wl_status_t status = WiFi.status();
  const int32_t rssi = WiFi.RSSI();
  LOG_DBG("KOSync", "%s: wifi_status=%d rssi=%ld ip=%s gw=%s dns=%s", stage, static_cast<int>(status),
          static_cast<long>(rssi), WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
          WiFi.dnsIP().toString().c_str());
}

// Base64 encode for HTTP Basic Auth
std::string base64Encode(const std::string& input) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  int val = 0, valb = -6;
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

// Verify there is enough contiguous heap to attempt a TLS handshake. The wolfSSL
// handshake peaks on a contiguous block during the ECC key exchange and chain
// verify. Total free heap can mislead because fragmentation leaves no single block
// big enough, which is precisely the scenario after recent PNG/JPG decode activity.
// Returns true if we should proceed; false means caller must abort with NETWORK_ERROR
// — in which case lastFailureDetail() reports the heap shortage instead of attempting
// a doomed handshake.
// NOTE: MIN_CONTIG_HEAP_FOR_TLS (26 KB) is sized from field telemetry — the
// wolfSSL handshake's measured peak contiguous demand is 12-16 KB (logged by the
// "TLS handshake heap" line below). See KOReaderSyncClient.h for the derivation.
bool checkHeapForTls() {
  const bool hasReusableSession = g_keepSessionOpen && g_sessionHttp != nullptr;
  const bool isUpload =
      (KOReaderSyncClient::lastOperation && strcmp(KOReaderSyncClient::lastOperation, "update progress") == 0);
  const unsigned requiredContig = KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS;

  // Upload can often reuse the already-established GET connection. In that case
  // a full handshake allocation is typically unnecessary, so avoid failing fast
  // on contiguous-heap threshold and let the HTTP client attempt reuse.
  if (isUpload && hasReusableSession) {
    return true;
  }

  // beginRequest() already populated lastContigHeapAtFailure for the diagnostic path.
  if (KOReaderSyncClient::lastContigHeapAtFailure + TLS_CONTIG_HEAP_TOLERANCE < requiredContig) {
    LOG_ERR("KOSync", "Insufficient contiguous heap for TLS: %u available, %u required",
            KOReaderSyncClient::lastContigHeapAtFailure, requiredContig);
    // Synthesize an esp_err_t-shaped value so the diagnostic detail string is uniform.
    KOReaderSyncClient::lastEspError = ESP_ERR_NO_MEM;
    return false;
  }
  return true;
}

void refreshHeapSnapshot() {
  KOReaderSyncClient::lastHeapAtFailure = ESP.getFreeHeap();
  KOReaderSyncClient::lastContigHeapAtFailure = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
}

void logTlsAttemptPlan(const char* operation, int attempt) {
  const bool isUpload = (operation && strcmp(operation, "update progress") == 0);
  const bool hasReusableSession = g_keepSessionOpen && g_sessionHttp != nullptr;
  const unsigned requiredContig = (isUpload && hasReusableSession) ? KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS_UPLOAD
                                                                   : KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS;

  LOG_DBG("KOSync", "%s attempt %d: keep_session=%s reusable_session=%s tls_mode=%s heap=%u contig=%u need=%u",
          operation ? operation : "request", attempt, g_keepSessionOpen ? "yes" : "no",
          hasReusableSession ? "yes" : "no", (isUpload && hasReusableSession) ? "reuse" : "handshake",
          KOReaderSyncClient::lastHeapAtFailure, KOReaderSyncClient::lastContigHeapAtFailure, requiredContig);
}

void resetSessionClientForRetry() {
  // Drop the kept-open connection so the next attempt reconnects cleanly.
  if (g_sessionHttp) {
    g_sessionHttp->close();
  }
}

void applyAuthHeaders(crosspoint::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  http.setBasicAuth(KOREADER_STORE.getUsername(), KOREADER_STORE.getPassword());
}

// Execute one KOSync request over wolfSSL (SecureHttpClient). Fills outBody with
// the response body and KOReaderSyncClient::lastHttpCode with the HTTP status.
// Returns an esp_err_t-shaped code so the callers' existing retry logic
// (ESP_OK / KO_ERR_CONNECT / KO_ERR_EAGAIN) is preserved:
//   ESP_OK              : got an HTTP response (any status)
//   KO_ERR_CONNECT: connect/transport failure (retryable)
//   KO_ERR_EAGAIN : timeout/truncated mid-transfer (retryable)
//   ESP_ERR_NO_MEM      : client alloc failure
// method: "GET" | "POST" | "PUT". contentType/body empty for GET.
esp_err_t performKoRequest(const char* method, const std::string& url, const char* contentType, const std::string& body,
                           std::string& outBody) {
  outBody.clear();
  KOReaderSyncClient::lastHttpCode = 0;

  crosspoint::SecureHttpClient* http = nullptr;
  crosspoint::SecureHttpClient localHttp;
  if (g_keepSessionOpen) {
    if (!g_sessionHttp) {
      g_sessionHttp = std::make_unique<crosspoint::SecureHttpClient>();
    }
    http = g_sessionHttp.get();
  } else {
    http = &localHttp;
  }

  // KOSync default host is https (TLS 1.3); custom local servers may be http.
  // SecureHttpClient verifies https against the curated roots and passes http
  // through a plain WiFiClient. Tiny JSON payloads, so the small-buffer intent
  // of the old 1 KB config is naturally met.
  //
  // Fail closed unless the user has explicitly disabled validation: applyAuthHeaders()
  // puts the sync account's password on the wire (Basic auth, plus the md5 in
  // x-auth-key), so accepting an unverifiable peer would hand those credentials to
  // whoever answered. No roots => SecureClient skips verification entirely, which is
  // what a self-hosted server with a private CA needs.
  http->setCACert(g_skipTlsValidation ? nullptr : CROSSPOINT_ROOTS_PEM);
  http->setAllowInsecureFallback(false);
  // Last-resort clock guard. The activities are expected to have run
  // HalClock::ensureUsableForTls() once WiFi came up (they have SETTINGS and therefore the
  // configured NTP server; this library does not). Asking here as well means a caller that
  // forgets does not get the old failure mode — a trust store that refuses to load, reported as
  // a bare "connect failed" with no verification error behind it. Chain, signature and hostname
  // stay fully enforced either way; only the validity window is waived.
  http->setAllowCertificateDateErrors(!HalClock::isPlausibleForTls());
  http->setTimeout(5000);
  // Sync replies are a few hundred bytes of JSON. A wrong server URL can answer with a whole web
  // page (a Booklore root serves its app's index.html with 200), and buffering all of it grew one
  // string until an allocation failed, which restarts the C3. Keep only the start: enough for
  // shouldRejectBody() to recognise HTML.
  // Ported from CrossInk commit 56a29f618 ("reject oversized KOReader authentication responses
  // (#783)", Julia Nguyen), which aborts the request instead; here the status still comes through.
  http->setMaxBufferedBody(KO_MAX_RESPONSE_BYTES);
  http->setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  http->clearHeaders();
  applyAuthHeaders(*http);
  if (contentType && *contentType) {
    http->addHeader("Content-Type", contentType);
  }

  // --- TLS handshake heap telemetry (gate health) ---
  // Reports the wolfSSL handshake's ACTUAL contiguous-heap trough vs the
  // MIN_CONTIG_HEAP_FOR_TLS gate, so the gate stays honest as the TLS stack /
  // cert chains evolve (it's what sized the current 26 KB — see the header).
  // handshakeMinLargest() moves only when a NEW handshake ran this request (it is
  // untouched on keep-alive reuse), so a change in its value flags a fresh
  // handshake and lets us pair the pre-request largest block with that trough.
  // Sampled with the same cap (MALLOC_CAP_DEFAULT) the handshake sampler uses so
  // the "consumed" delta is apples-to-apples. Kept at INFO so it appears in field
  // bug reports (handshakes are infrequent — one per sync).
  const size_t troughBefore = http->lastHandshakeMinLargest();
  const size_t preflightLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

  const uint32_t requestStartMs = millis();
  const int rc = http->request(method, url, body);
  const uint32_t elapsedMs = millis() - requestStartMs;

  const size_t troughLargest = http->lastHandshakeMinLargest();
  const bool didHandshake = (troughLargest != troughBefore) && (troughLargest != SIZE_MAX);
  if (didHandshake) {
    const long consumedContig = static_cast<long>(preflightLargest) - static_cast<long>(troughLargest);
    LOG_INF("KOSync",
            "TLS handshake heap: preLargest=%u troughLargest=%u consumedContig=%ld troughFree=%u | gate=%u insecure=%d",
            static_cast<unsigned>(preflightLargest), static_cast<unsigned>(troughLargest), consumedContig,
            static_cast<unsigned>(http->lastHandshakeMinFree()), KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS,
            static_cast<int>(http->lastConnectionWasInsecure()));
  }

  if (rc >= 100) {  // got an HTTP status line
    KOReaderSyncClient::lastHttpCode = rc;
    outBody = http->getBody();
    if (http->bodyTruncated()) {
      LOG_ERR("KOSync", "Response to %s %s is over %u bytes; kept only the start", method, url.c_str(),
              static_cast<unsigned>(KO_MAX_RESPONSE_BYTES));
    }
    logResponseBody(method, url, rc, outBody, elapsedMs, didHandshake);
    return ESP_OK;
  }
  // Time a failed request too: a 5000 ms elapsed against the 5000 ms timeout says the peer
  // went quiet, where a sub-second failure is a refused connect or a TLS rejection.
  LOG_ERR("KOSync", "Request %s %s failed after %lu ms (rc=%d, %s)", method, url.c_str(),
          static_cast<unsigned long>(elapsedMs), rc, didHandshake ? "fresh handshake" : "no handshake");
  // Map SecureHttpClient transport errors to the esp_err_t codes the callers
  // already branch on for retry decisions.
  switch (rc) {
    case crosspoint::SecureHttpClient::ERR_CONNECT:
    case crosspoint::SecureHttpClient::ERR_BAD_URL:
      return KO_ERR_CONNECT;
    case crosspoint::SecureHttpClient::ERR_TIMEOUT:
    case crosspoint::SecureHttpClient::ERR_TRUNCATED:
    case crosspoint::SecureHttpClient::ERR_SEND:
      return KO_ERR_EAGAIN;
    default:
      return ESP_FAIL;
  }
}
}  // namespace

// Returns true if credentials are present; logs and returns false otherwise.
static inline bool hasCredentials() {
  if (KOREADER_STORE.hasCredentials()) return true;
  LOG_INF("KOSync", "No credentials configured");
  return false;
}

void KOReaderSyncClient::setSkipTlsValidation(bool skip) { g_skipTlsValidation = skip; }

void KOReaderSyncClient::beginPersistentSession() {
  if (g_keepSessionOpen) {
    return;
  }
  g_keepSessionOpen = true;
  g_sessionHttp.reset();  // fresh client; created lazily on first request
}

void KOReaderSyncClient::endPersistentSession() {
  g_keepSessionOpen = false;
  g_sessionHttp.reset();  // closes the kept-open TLS connection
}

KOReaderSyncClient::Error KOReaderSyncClient::registerUser() {
  if (!hasCredentials()) return NO_CREDENTIALS;

  beginRequest("register");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_INF("KOSync", "Registering user: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Register request body: <redacted credentials>");

  std::string responseBody;
  const esp_err_t err = performKoRequest("POST", url, "application/json", body, responseBody);
  const int httpCode = lastHttpCode;
  lastEspError = err;

  LOG_DBG("KOSync", "Register response: %d (err: %s) | body: %s", httpCode, esp_err_to_name(err), responseBody.c_str());

  if (err != ESP_OK) {
    return NETWORK_ERROR;
  }

  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;

  // A 2xx carrying HTML is a captive portal, not a created account. Check before the status
  // mapping below, or a portal's 200 would be reported as "username already taken".
  // Only 2xx is guarded: the 402 branch deliberately reads a human-readable body, so a server
  // that words its errors in plain text must still reach it.
  if (httpCode >= 200 && httpCode < 300 && shouldRejectBody(httpCode, responseBody)) return INVALID_RESPONSE;

  if (httpCode == 200) {
    // Some server implementations return 200 when the user already exists
    return USER_EXISTS;
  } else if (httpCode >= 200 && httpCode < 300) {
    // Any other 2xx means the account was created. The reference kosync server
    // answers 201, but KOSync-compatible implementations differ (BookLore/grimmory
    // is a Spring service and uses the idiomatic 204).
    return OK;
  } else if (httpCode == 402) {
    // Both "user already exists" (error 2002) and "registration disabled" (error 2005)
    // return HTTP 402 on the original kosync server. Distinguish them by body text.
    std::string lowerBody = responseBody;
    std::transform(lowerBody.begin(), lowerBody.end(), lowerBody.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerBody.find("already") != std::string::npos) {
      return USER_EXISTS;
    }
    return REGISTRATION_DISABLED;
  } else if (httpCode == 409) {
    // korrosync returns 409 for existing users
    return USER_EXISTS;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  if (!hasCredentials()) return NO_CREDENTIALS;

  beginRequest("auth");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  std::string responseBody;
  const esp_err_t err = performKoRequest("GET", url, nullptr, "", responseBody);
  const int httpCode = lastHttpCode;
  lastEspError = err;

  LOG_DBG("KOSync", "Auth response: %d (err: %s)", httpCode, esp_err_to_name(err));

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;
  // Reject a non-JSON body whatever the status. Screening only 2xx would let a captive
  // portal's 401 + HTML surface as AUTH_FAILED, sending the user off to reset credentials
  // that were fine.
  if (shouldRejectBody(httpCode, responseBody)) return INVALID_RESPONSE;
  // A 2xx that survived the body screen is a successful authentication, bodiless or not.
  // Requiring a body on anything but 204/205 locked out servers that acknowledge auth with a
  // bare 200 — and reported them as a captive portal, sending users to reset working
  // credentials. A portal cannot reach here: it serves a page, which the screen above rejects.
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  if (!hasCredentials()) return NO_CREDENTIALS;

  beginRequest("get progress");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  // Calibre-Web NextGen withholds a percentage-only position (one its web reader or a Kobo
  // wrote) from a client that does not say it can seek to one. Servers without the parameter
  // route on the path and ignore it. Ported from crosspoint-reader PR #3219 (Micah Halter,
  // @mehalter).
  std::string url =
      KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash + "?position_kinds=locator,percentage";
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  std::string responseBody;
  esp_err_t err = ESP_FAIL;
  int httpCode = 0;

  for (int attempt = 1; attempt <= 3; attempt++) {
    // Retry attempts can happen after memory churn from a failed handshake.
    // Refresh heap snapshot each pass so preflight and diagnostics use current values.
    refreshHeapSnapshot();
    logTlsAttemptPlan("get progress", attempt);
    if (!checkHeapForTls()) {
      return NETWORK_ERROR;
    }

    logWifiSnapshot("WiFi before getProgress");
    err = performKoRequest("GET", url, nullptr, "", responseBody);
    httpCode = lastHttpCode;
    lastEspError = err;

    LOG_DBG("KOSync", "GET %s -> %d (err: %s) [attempt %d body_len=%u]", url.c_str(), httpCode, esp_err_to_name(err),
            attempt, static_cast<unsigned>(responseBody.size()));
    if (err == ESP_OK && (httpCode < 200 || httpCode >= 300)) {
      rememberResponsePreview(responseBody.c_str());
      LOG_ERR("KOSync", "GET failure body preview: %s", g_lastResponsePreview);
    }

    // Retry up to two times for transient connect or EAGAIN failures only.
    // Why: this recovers short AP/roaming or temporary I/O hiccups without masking
    // persistent TLS/auth/server errors that should be surfaced immediately.
    const bool retryable = (err == KO_ERR_CONNECT || err == KO_ERR_EAGAIN);
    if (err == ESP_OK || !retryable || attempt == 3) {
      break;
    }

    // Failed connect/EAGAIN can leave the kept-open connection in a bad state.
    // Drop it before retry so we don't repeat work on a stale transport.
    resetSessionClientForRetry();

    LOG_ERR("KOSync", "getProgress request failed on attempt %d, retrying", attempt);
    logWifiSnapshot("WiFi before getProgress retry");
    delay(400 * attempt);
  }

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;

  // A captive portal or proxy serving HTML makes the status code meaningless, and here that
  // is worse than a bad error message: its 404 would read as "no progress stored", and the
  // caller answers that by uploading local progress into the void. Reject on body shape
  // before any status is interpreted. 401 is included so a portal cannot masquerade as an
  // authentication failure and send the user off resetting working credentials.
  if (shouldRejectBody(httpCode, responseBody)) return INVALID_RESPONSE;

  if (httpCode >= 200 && httpCode < 300) {
    // A bodiless 2xx means the server has no stored progress for this document:
    // Spring-based KOSync implementations answer 204 where the reference server
    // answers 200 with an empty object. Both belong on the same graceful
    // no-remote-progress path as 404, not in SERVER_ERROR.
    if (bodyIsEffectivelyEmpty(responseBody)) {
      LOG_INF("KOSync", "HTTP %d with no body — treating as not found", httpCode);
      return NOT_FOUND;
    }
    return parseProgressReply(responseBody, documentHash, outProgress);
  }

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) {
    LOG_INF("KOSync", "GET progress returned 404 for %s - treating as NOT_FOUND", url.c_str());
    return NOT_FOUND;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  if (!hasCredentials()) return NO_CREDENTIALS;

  beginRequest("update progress");
  if (!checkHeapForTls()) return NETWORK_ERROR;

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u, contig: %u)", url.c_str(), lastHeapAtFailure,
          lastContigHeapAtFailure);

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  if (progress.metadata) {
    JsonObject meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
    if (!progress.metadata->isbn.empty()) meta["isbn"] = progress.metadata->isbn;
    if (!progress.metadata->asin.empty()) meta["asin"] = progress.metadata->asin;
    if (!progress.metadata->series.empty()) meta["series"] = progress.metadata->series;
    // A number; an index that is not one is left out rather than sent as text.
    const std::string& index = progress.metadata->seriesIndex;
    char* end = nullptr;
    const float number = index.empty() ? 0.0f : std::strtof(index.c_str(), &end);
    if (end != nullptr && end != index.c_str() && *end == '\0' && std::isfinite(number)) {
      meta["series_index"] = number;
    }
  }

  std::string body;
  serializeJson(doc, body);

  LOG_INF("KOSync", "Request body: %s", body.c_str());

  std::string responseBody;
  esp_err_t err = ESP_FAIL;
  int httpCode = 0;

  for (int attempt = 1; attempt <= 3; attempt++) {
    // Retry attempts can happen after memory churn from a failed handshake.
    // Refresh heap snapshot each pass so preflight and diagnostics use current values.
    refreshHeapSnapshot();
    logTlsAttemptPlan("update progress", attempt);
    if (!checkHeapForTls()) {
      return NETWORK_ERROR;
    }

    logWifiSnapshot("WiFi before updateProgress");
    err = performKoRequest("PUT", url, "application/json", body, responseBody);
    httpCode = lastHttpCode;
    lastEspError = err;

    LOG_DBG("KOSync", "PUT %s -> %d (err: %s) [attempt %d body_len=%u]", url.c_str(), httpCode, esp_err_to_name(err),
            attempt, static_cast<unsigned>(responseBody.size()));
    if (err == ESP_OK && (httpCode < 200 || httpCode >= 300)) {
      rememberResponsePreview(responseBody.c_str());
      LOG_ERR("KOSync", "PUT failure body preview: %s", g_lastResponsePreview);
      LOG_ERR("KOSync", "PUT failure request summary: document=%s percentage=%.4f progress=%s",
              progress.document.c_str(), progress.percentage, progress.progress.c_str());
    }

    // Retry up to two times for transient connect or EAGAIN failures only.
    // Why: same policy as GET keeps behavior predictable across both endpoints.
    const bool retryable = (err == KO_ERR_CONNECT || err == KO_ERR_EAGAIN);
    if (err == ESP_OK || !retryable || attempt == 3) {
      break;
    }

    // Failed connect/EAGAIN can leave the kept-open connection in a bad state.
    // Drop it before retry so we don't repeat work on a stale transport.
    resetSessionClientForRetry();

    LOG_ERR("KOSync", "updateProgress request failed on attempt %d, retrying", attempt);
    logWifiSnapshot("WiFi before updateProgress retry");
    delay(400 * attempt);
  }

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode >= 300 && httpCode < 400) return REDIRECT_ERROR;
  // Same reasoning as getProgress: HTML here means the response is not from the API, so
  // neither a 2xx "accepted" nor a 4xx "rejected" reading of the status is trustworthy.
  if (shouldRejectBody(httpCode, responseBody)) return INVALID_RESPONSE;
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::lastFailureDetail() {
  const bool isUpload = (lastOperation && strcmp(lastOperation, "update progress") == 0);
  const bool hasReusableSession = g_keepSessionOpen && g_sessionHttp != nullptr;
  const unsigned requiredContig =
      (isUpload && hasReusableSession) ? MIN_CONTIG_HEAP_FOR_TLS_UPLOAD : MIN_CONTIG_HEAP_FOR_TLS;

  // Heap-pressure case: surfaced when checkHeapForTls() refused before any TCP/TLS work happened.
  if (lastEspError == ESP_ERR_NO_MEM && lastHttpCode == 0) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
             "%s: low memory (%u free, %u contig, need %u). Reboot device.", lastOperation, lastHeapAtFailure,
             lastContigHeapAtFailure, requiredContig);
    return g_failureDetailBuf;
  }
  // Network/TLS case: the request failed before getting a status code.
  if (lastHttpCode == 0 && lastEspError != 0) {
    // HTTPS connect failures are usually DNS, cert, or plain network problems
    // (wolfSSL negotiates TLS 1.3/1.2, so version mismatch is not the cause).
    // Include the error name and heap stats so the user/bug-report can triage.
    if (lastEspError == KO_ERR_CONNECT && KOREADER_STORE.getBaseUrl().rfind("https", 0) == 0) {
      snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
               "%s: connect failed — check network, DNS, or certificate (heap %u/%u contig)", lastOperation,
               lastHeapAtFailure, lastContigHeapAtFailure);
    } else {
      snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf), "%s: %s (heap %u/%u contig)", lastOperation,
               esp_err_to_name(lastEspError), lastHeapAtFailure, lastContigHeapAtFailure);
    }
    return g_failureDetailBuf;
  }
  // Invalid-response case: the body was not JSON (e.g. captive portal HTML). Flagged
  // explicitly rather than inferred from the status, because this is now rejected under 404
  // and 401 too, where the status alone would look like an ordinary API answer.
  if (g_lastInvalidBody) {
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
             "%s: HTTP %d but the body was not JSON (captive portal or proxy?)", lastOperation, lastHttpCode);
    return g_failureDetailBuf;
  }
  // Server case: got an HTTP status the client didn't recognize as success.
  if (lastHttpCode != 0) {
    if (lastHttpCode == 404 && lastOperation && strcmp(lastOperation, "update progress") == 0) {
      std::string lowerPreview = g_lastResponsePreview;
      std::transform(lowerPreview.begin(), lowerPreview.end(), lowerPreview.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lowerPreview.find("book not found") != std::string::npos) {
        snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
                 "%s: server does not know this book yet; server expects the same file to already exist there, usually "
                 "downloaded via OPDS",
                 lastOperation);
        return g_failureDetailBuf;
      }
      snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf),
               "%s: HTTP 404 (upload rejected; server may require book to be known)", lastOperation);
      return g_failureDetailBuf;
    }
    snprintf(g_failureDetailBuf, sizeof(g_failureDetailBuf), "%s: HTTP %d", lastOperation, lastHttpCode);
    return g_failureDetailBuf;
  }
  // No prior request, or success.
  g_failureDetailBuf[0] = '\0';
  return g_failureDetailBuf;
}

const char* KOReaderSyncClient::errorString(Error error) {
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
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case USER_EXISTS:
      return "Username is already taken";
    case REGISTRATION_DISABLED:
      return "Registration is disabled on this server";
    case REDIRECT_ERROR:
      return "Server redirected (check server URL)";
    case INVALID_RESPONSE:
      return "Unexpected response (check server URL)";
    default:
      return "Unknown error";
  }
}
