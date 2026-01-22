#pragma once
#include <SDCardManager.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps WiFiClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);

 private:
  static constexpr size_t DOWNLOAD_CHUNK_SIZE = 1024;
};
