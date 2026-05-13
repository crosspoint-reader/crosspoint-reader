#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>

#include <cstring>
#include <memory>
#include <utility>

#include "util/UrlUtils.h"

namespace {
class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress)
      : file_(file), total_(total), progress_(std::move(progress)), abortRequested_(false) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Write-through stream for HTTPClient::writeToStream with progress tracking.
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    if (progress_) {
      if (!progress_(downloaded_, total_)) {
        abortRequested_ = true;
        return 0;
      }
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }
  bool aborted() const { return abortRequested_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  bool abortRequested_ = false;
  HttpDownloader::ProgressCallback progress_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  http.addHeader("Connection", "close");

  if (!username.empty() || !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    client->stop();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();
  if (client) {
    client->stop();
  }

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  StreamString stream;
  if (!fetchUrl(url, stream, username, password)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const std::string& username,
                                                             const std::string& password) {
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  http.addHeader("Connection", "close");

  if (!username.empty() || !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    client->stop();
    return HTTP_ERROR;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    http.end();
    return FILE_ERROR;
  }

  LOG_DBG("HTTP", "Opened destination file for writing: %s", destPath.c_str());

  int writeResult = -1;
  size_t downloaded = 0;
  bool writeOk = true;

  if (contentLength > 0) {
    NetworkClient& stream = http.getStream();
    uint8_t buffer[1024];
    writeResult = 1;
    bool aborted = false;
    unsigned long lastAvailLog = millis();
    unsigned long startMs = millis();
    unsigned long lastProgressPoll = millis();

    while (http.connected() && downloaded < contentLength) {
      size_t available = stream.available();
      if (available > 0) {
        size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
        if (downloaded + toRead > contentLength) {
          toRead = contentLength - downloaded;
        }
        int readSize = stream.readBytes(reinterpret_cast<char*>(buffer), toRead);
        if (readSize > 0) {
          if (file.write(buffer, readSize) != static_cast<size_t>(readSize)) {
            LOG_ERR("HTTP", "File write failed: wrote %d/%zu bytes to %s", readSize, toRead, destPath.c_str());
            writeOk = false;
            writeResult = -1;
            break;
          }
          downloaded += readSize;
          if (progress && !progress(downloaded, contentLength)) {
            LOG_DBG("HTTP", "Download aborted by callback at %zu/%zu", downloaded, contentLength);
            aborted = true;
            break;
          }
        } else {
          LOG_ERR("HTTP", "Stream readBytes returned %d after %zu bytes", readSize, downloaded);
          break;
        }
      } else {
        if (millis() - lastProgressPoll > 100) {
          if (progress && !progress(downloaded, contentLength)) {
            LOG_DBG("HTTP", "Download aborted by callback while waiting for data at %zu/%zu", downloaded,
                    contentLength);
            aborted = true;
            break;
          }
          lastProgressPoll = millis();
        }
        if (millis() - lastAvailLog > 2000) {
          LOG_DBG("HTTP", "Waiting for available data: downloaded=%zu connected=%d elapsed=%lums", downloaded,
                  http.connected(), millis() - startMs);
          lastAvailLog = millis();
        }
        delay(1);
      }
    }

    if (aborted) {
      file.flush();
      file.close();
      http.end();
      client->stop();
      Storage.remove(destPath.c_str());
      return ABORTED;
    }

    if (downloaded != contentLength) {
      LOG_ERR("HTTP", "Download size mismatch after loop: got %zu expected %zu", downloaded, contentLength);
      writeResult = -1;
    }
  } else {
    FileWriteStream fileStream(file, contentLength, progress);
    writeResult = http.writeToStream(&fileStream);
    downloaded = fileStream.downloaded();
    writeOk = fileStream.ok();
    if (fileStream.aborted()) {
      file.flush();
      file.close();
      http.end();
      client->stop();
      Storage.remove(destPath.c_str());
      return ABORTED;
    }
  }

  // Flush before closing to ensure data is written to the SD card.
  // Without this, Storage.exists() might return false immediately after
  // even though the file was written (FAT not yet updated on disk).
  file.flush();
  file.close();
  http.end();
  client->stop();

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream error: %d (downloaded %zu)", writeResult, downloaded);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!writeOk) {
    LOG_ERR("HTTP", "Write failed during download (downloaded %zu)", downloaded);
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
