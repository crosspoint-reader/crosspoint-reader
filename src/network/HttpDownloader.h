#pragma once
#include <HalStorage.h>

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

  enum AssetType {
    WEB_ASSETS,
    // Future: FONT_ASSETS, IMAGE_ASSETS, etc.
  };

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @param username Optional username for HTTP Basic authentication
   * @param password Optional password for HTTP Basic authentication
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent,
                       const char* username = nullptr, const char* password = nullptr);

  static bool fetchUrl(const std::string& url, Stream& stream,
                       const char* username = nullptr, const char* password = nullptr);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @param username Optional username for HTTP Basic authentication
   * @param password Optional password for HTTP Basic authentication
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr,
                                      const char* username = nullptr, const char* password = nullptr);

  /**
   * Ensure a list of assets are available on SD card, downloading missing ones.
   * Supports both text and binary files. Base paths are determined by asset type.
   * 
   * Example usage:
   *   const char* webAssets[] = {"HomePage.html", "FilesPage.html", "SettingsPage.html", nullptr};
   *   HttpDownloader::ensureAssetsAvailable(WEB_ASSETS, webAssets, "WEB");
   *   
   *   // Future usage for other asset types:
   *   const char* fontAssets[] = {"font.ttf", "icons.woff2", nullptr};
   *   HttpDownloader::ensureAssetsAvailable(FONT_ASSETS, fontAssets, "FONT");
   * 
   * @param assetType The type of assets (determines base paths)
   * @param assetNames Array of asset filenames to check/download (null-terminated, relative to base paths)
   * @param loggerPrefix Prefix for log messages (e.g., "WEB", "FONTS")
   * @return true if all assets are available, false if any download failed
   */
  static bool ensureAssetsAvailable(AssetType assetType, const char* const* assetNames, const char* loggerPrefix);

 private:
  static constexpr size_t DOWNLOAD_CHUNK_SIZE = 1024;
};
