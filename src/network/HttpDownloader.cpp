#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <StreamString.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

#define STRINGIFY(x) #x

// Compile-time repository URL for asset downloads
// To override for forks, modify this URL or use build flags
const char* ASSET_REPO_URL = "https://raw.githubusercontent.com/crosspoint-reader/crosspoint-reader/master";

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent,
                           const char* username, const char* password) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are provided
  if (username && password && strlen(username) > 0 && strlen(password) > 0) {
    std::string credentials = std::string(username) + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent,
                           const char* username, const char* password) {
  StreamString stream;
  if (!fetchUrl(url, stream, username, password)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress,
                                                             const char* username, const char* password) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are provided
  if (username && password && strlen(username) > 0 && strlen(password) > 0) {
    std::string credentials = std::string(username) + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const size_t contentLength = http.getSize();
  LOG_DBG("HTTP", "Content-Length: %zu", contentLength);

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Get the stream for chunked reading
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERR("HTTP", "Failed to get stream");
    file.close();
    Storage.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Download in chunks
  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      LOG_ERR("HTTP", "Write failed: wrote %zu of %zu bytes", written, bytesRead);
      file.close();
      Storage.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;

    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}

bool HttpDownloader::ensureAssetsAvailable(AssetType assetType, const char* const* assetNames, const char* loggerPrefix) {
  // Determine base paths based on asset type
  const char* sdBasePath;
  const char* assetSubPath;
  
  switch (assetType) {
    case WEB_ASSETS:
      sdBasePath = "/web/";
      assetSubPath = "/data/web/";
      break;
    // Future asset types can be added here:
    // case FONT_ASSETS:
    //   sdBasePath = "/fonts/";
    //   assetSubPath = "/data/fonts/";
    //   break;
    default:
      LOG_ERR(loggerPrefix, "Unknown asset type: %d", assetType);
      return false;
  }

  // Construct GitHub base URL using compile-time repository URL
  String githubBaseUrl = String(ASSET_REPO_URL) + assetSubPath;

  bool allAssetsAvailable = true;

  // Check if all assets exist
  for (size_t i = 0; assetNames[i] != nullptr; ++i) {
    const char* assetName = assetNames[i];
    String fullSdPath = String(sdBasePath) + assetName;
    
    if (!Storage.exists(fullSdPath.c_str())) {
      allAssetsAvailable = false;
      LOG_DBG(loggerPrefix, "Missing asset: %s", fullSdPath.c_str());
      break;
    }
  }

  if (allAssetsAvailable) {
    LOG_DBG(loggerPrefix, "All assets are available on SD card");
    return true;
  }

  LOG_INF(loggerPrefix, "Downloading missing assets...");

  // Download missing assets
  for (size_t i = 0; assetNames[i] != nullptr; ++i) {
    const char* assetName = assetNames[i];
    String fullSdPath = String(sdBasePath) + assetName;
    
    if (Storage.exists(fullSdPath.c_str())) {
      LOG_DBG(loggerPrefix, "Asset already exists: %s", fullSdPath.c_str());
      continue;
    }

    // Construct download URL
    String downloadUrl = githubBaseUrl + assetName;

    LOG_INF(loggerPrefix, "Downloading %s from %s", fullSdPath.c_str(), downloadUrl.c_str());

    // Download directly to SD card
    DownloadError error = downloadToFile(std::string(downloadUrl.c_str()), std::string(fullSdPath.c_str()));

    if (error != OK) {
      LOG_ERR(loggerPrefix, "Failed to download %s (error: %d)", downloadUrl.c_str(), error);
      return false;
    }

    LOG_INF(loggerPrefix, "Successfully downloaded: %s", fullSdPath.c_str());
  }

  LOG_INF(loggerPrefix, "All assets downloaded successfully");
  return true;
}
