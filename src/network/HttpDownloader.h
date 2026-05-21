#pragma once
#include <HalStorage.h>

#include <functional>
#include <memory>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme). Ported
 * from upstream PR #2075.
 */
class HttpDownloader {
 public:
  // Progress callback. Return false to abort the transfer.
  using ProgressCallback = std::function<bool(unsigned int downloaded, unsigned int total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Reusable HTTP+TLS session. Holding one of these across multiple
   * downloadToFile() calls keeps a single esp_http_client_handle_t alive,
   * so the TLS handshake (≈36 KB of contiguous mbedtls buffers, RSA chain
   * verify, etc.) runs once instead of per-file. This is the structural fix
   * for back-to-back HTTPS calls failing on a fragmented heap.
   *
   * Usage: construct one, pass to downloadToFile(session, …) for every file
   * served by the same host. Destroying it closes the connection.
   *
   * Cross-host reuse is technically supported (esp_http_client_set_url tears
   * down and reopens) but defeats the heap win — group calls by host.
   */
  class Session {
   public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    struct Impl;
    Impl* impl() const { return impl_.get(); }

   private:
    std::unique_ptr<Impl> impl_;
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const std::string& username = "",
                                      const std::string& password = "");

  /**
   * Session-based variant. The first call on a fresh session opens the
   * connection (TLS handshake, cert verification, etc.); subsequent calls to
   * URLs on the same host reuse the open client and skip the handshake.
   */
  static DownloadError downloadToFile(Session& session, const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const std::string& username = "",
                                      const std::string& password = "");
};
