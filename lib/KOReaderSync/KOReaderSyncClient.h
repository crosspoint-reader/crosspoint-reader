#pragma once
#include <optional>
#include <string>

/**
 * Optional document metadata sent alongside progress uploads.
 * Only populated when KOReaderCredentialStore::getSendMetadata() is true.
 * The official sync server ignores this field; custom servers may use it.
 */
struct KOReaderMetadata {
  std::string filename;
  std::string title;
  std::string authors;
  // Sent only when known. The field names follow crosspoint-reader PR #3804, so a server written
  // for its extended metadata reads ours.
  std::string isbn;
  std::string asin;
  std::string series;
  std::string seriesIndex;  // as the book writes it ("3", "2.5"); sent as a number
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                      // Document hash
  std::string progress;                      // XPath-like progress string; empty: percentage only
  float percentage = 0.0f;                   // Progress percentage (0.0 to 1.0)
  std::string device;                        // Device name
  std::string deviceId;                      // Device ID
  int64_t timestamp = 0;                     // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;  // Optional document metadata
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   POST /users/create - Register a new user
 *   GET  /users/auth  - Authenticate (validate credentials)
 *   GET  /syncs/progress/:document - Get progress for a document
 *   PUT  /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  /**
   * Accept any TLS certificate on the sync connection (mirrors the app's
   * "skip HTTPS validation" setting, which this library cannot read itself).
   * The app pushes the current value before it uses the client. Off by default:
   * the sync request carries the account password, so an unverified peer gets
   * the credentials.
   */
  static void setSkipTlsValidation(bool skip);

  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    USER_EXISTS,
    REGISTRATION_DISABLED,
    REDIRECT_ERROR,
    INVALID_RESPONSE
  };

  /**
   * Register a new user account with the sync server.
   * Uses credentials already stored in KOReaderCredentialStore.
   * @return OK on success, USER_EXISTS if taken, REGISTRATION_DISABLED if server disallows it
   */
  static Error registerUser();

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * Keep HTTP/TLS session alive across multiple sync requests (GET/PUT).
   * Intended for KOReaderSyncActivity to reduce repeated handshake churn.
   */
  static void beginPersistentSession();

  /**
   * Close and release any persistent HTTP/TLS session.
   */
  static void endPersistentSession();

  /**
   * Get human-readable error message (short, for status line).
   */
  static const char* errorString(Error error);

  /**
   * Get a detailed diagnostic string for the last failure, combining the error
   * category, the underlying esp_err_t (when applicable), the HTTP status code,
   * and the free heap at the time of failure. Intended for the SYNC_FAILED screen
   * so users and bug-reporters can tell network/TLS/server failures apart.
   * Returns a stable c-string valid until the next request.
   */
  static const char* lastFailureDetail();

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;
  /** Last transport result, esp_err_t-shaped (ESP_OK if the request reached the
   *  server; KO_ERR_CONNECT / KO_ERR_EAGAIN / ESP_ERR_NO_MEM otherwise). */
  static int lastEspError;
  /** Free heap (bytes) captured at the start of the last failing request. */
  static unsigned lastHeapAtFailure;
  /** Largest contiguous free block (bytes) at the start of the last failing request. */
  static unsigned lastContigHeapAtFailure;
  /** Operation tag set at the start of each request, surfaced in lastFailureDetail. */
  static const char* lastOperation;

  /**
   * Minimum largest-contiguous-free heap block (bytes) required before attempting
   * a request. Below this, the client refuses with NETWORK_ERROR and lastFailureDetail
   * reports a heap-pressure message instead of attempting a doomed TLS handshake.
   *
   * Sized from field telemetry (the "TLS handshake heap" INFO line in
   * performKoRequest): the wolfSSL TLS 1.3 handshake's peak contiguous demand was
   * 12 KB (sync.koreader.rocks) to 16 KB (kosync.rustysoft.de) across register/GET/PUT.
   * 16 KB is effectively the ceiling — it is wolfSSL's max TLS input-record buffer.
   * 26 KB leaves ~10 KB margin over that worst case (for cert-chain variance and any
   * transient bignum peak the sampler may miss between wolfSSL_connect() iterations).
   * Was 36 KB, an mbedTLS-era figure that refused syncs a wolfSSL handshake would
   * have completed comfortably.
   */
  static constexpr unsigned MIN_CONTIG_HEAP_FOR_TLS = 26 * 1024;
  // Relaxed threshold only for upload when reusing an already-established session.
  // Uploads that must perform a fresh handshake still require MIN_CONTIG_HEAP_FOR_TLS.
  static constexpr unsigned MIN_CONTIG_HEAP_FOR_TLS_UPLOAD = 24 * 1024;
};
