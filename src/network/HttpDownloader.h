#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>
#include <vector>

// A single extra header sent with a request, on top of whatever auth/UA
// headers the transport already sets (e.g. a Cloudflare Access service token).
struct HttpHeader {
  std::string name;
  std::string value;
};

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  // Pre-flight floor for starting a TLS transfer. Below this the session or
  // its ~17KB record buffer fails mid-stream (wolfSSL MEMORY_E) — or an
  // interior allocation abort()s the device. Callers should check before
  // downloadToFile() and fail into their error UI instead.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;

  /**
   * Fetch text content from a URL with optional credentials and extra headers.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", const std::vector<HttpHeader>& customHeaders = {});

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", const std::vector<HttpHeader>& customHeaders = {});

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", const std::vector<HttpHeader>& customHeaders = {});

  /**
   * Download a file to the SD card with optional credentials and extra headers.
   *
   * downgradeRedirectsToHttp rewrites followed redirect targets from https to
   * http so the bulk transfer skips a second TLS session (and its ~17KB record
   * buffer — the OOM site on low-heap C3 boards).
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::vector<HttpHeader>& customHeaders = {},
                                      bool downgradeRedirectsToHttp = false);
};
