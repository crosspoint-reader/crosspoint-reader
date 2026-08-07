#pragma once
#include <SecureHttpClient.h>

#include <cstddef>
#include <string>

/**
 * Minimal WebDAV client for uploading highlight files to Koofr.
 *
 * Base URL: https://app.koofr.net/dav/Koofr (overridable, so any WebDAV server
 * works) with HTTP Basic authentication. Koofr rejects the account login
 * password on this endpoint — the stored password must be an app password
 * created in Koofr's web preferences.
 *
 * Methods used:
 *   MKCOL <dir>   - create a collection (405 means it already exists)
 *   PUT   <file>  - create or overwrite a file
 *
 * Instances hold one keep-alive connection, so uploading a folder of
 * highlights pays for a single TLS handshake instead of one per file. Allocate
 * on the heap (makeUniqueNoThrow): the embedded SecureHttpClient is larger than
 * the stack budget allows.
 */
class KoofrClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    LOW_MEMORY,
    FILE_ERROR,     // local file could not be opened or read
    FILE_TOO_LARGE  // local file exceeds what can be buffered for a request body
  };

  KoofrClient();

  /**
   * Create the configured destination folder and every parent above it.
   * Idempotent: folders that already exist are left alone.
   */
  Error ensureRemoteDir();

  /**
   * Upload one local file into the configured destination folder, overwriting
   * any remote file of the same name.
   * @param localPath Absolute path on the SD card
   * @param remoteName File name to write remotely (not a path; no separators)
   */
  Error uploadFile(const char* localPath, const std::string& remoteName);

  static const char* errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  int lastHttpCode = 0;

  /** Largest file this client will upload; see the note in the .cpp. */
  static constexpr size_t MAX_UPLOAD_BYTES = 24 * 1024;

 private:
  freeink::SecureHttpClient http;

  // Prepares http for a request against `url`: clears per-request state and
  // reapplies auth. Returns false on a malformed URL.
  bool beginRequest(const std::string& url);

  // Maps an HTTP status onto an Error, treating `success` codes as OK.
  Error classify(int httpCode) const;
};
