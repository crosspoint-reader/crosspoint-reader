#include "network/BufferedHttpUpload.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "SpiBusMutex.h"

namespace network {

BufferedHttpUploadSession& sharedBufferedHttpUploadSession() {
  static BufferedHttpUploadSession session;
  return session;
}

bool BufferedHttpUploadSession::flushBuffer(const char* logLabel) {
  if (uploadBufferPos > 0 && uploadFile) {
    SpiBusMutex::Guard guard;
    esp_task_wdt_reset();
    const unsigned long writeStart = millis();
    const size_t written = uploadFile.write(uploadBuffer, uploadBufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();

    if (written != uploadBufferPos) {
      LOG_DBG("WEB", "[%s] Buffer flush failed: expected %u, wrote %u", logLabel,
              static_cast<unsigned int>(uploadBufferPos), static_cast<unsigned int>(written));
      uploadBufferPos = 0;
      return false;
    }

    uploadBufferPos = 0;
  }

  return true;
}

void BufferedHttpUploadSession::handleUpload(WebServer* server, const BufferedHttpUploadConfig& config) {
  if (server == nullptr) {
    return;
  }

  const char* logLabel = (config.logLabel != nullptr) ? config.logLabel : "UPLOAD";
  const char* storageTag = (config.storageTag != nullptr) ? config.storageTag : logLabel;

  esp_task_wdt_reset();

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    esp_task_wdt_reset();

    uploadFileName = upload.filename;
    uploadPathValue = "/";
    targetFilePath = "";
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    uploadBufferPos = 0;
    uploadStartTime = millis();
    totalWriteTime = 0;
    writeCount = 0;
    uploadLastLoggedSize = 0;

    BufferedHttpUploadTarget target{};
    if (config.resolveTarget == nullptr || !config.resolveTarget(server, uploadFileName, target, uploadError)) {
      return;
    }

    uploadPathValue = target.uploadPath.isEmpty() ? "/" : target.uploadPath;
    targetFilePath = target.filePath;
    if (targetFilePath.isEmpty()) {
      uploadError = "Missing upload target";
      return;
    }

    LOG_DBG("WEB", "[%s] START: %s to path: %s", logLabel, uploadFileName.c_str(), uploadPathValue.c_str());
    LOG_DBG("WEB", "[%s] Free heap: %d bytes", logLabel, ESP.getFreeHeap());

    bool hadExistingFile = false;
    esp_task_wdt_reset();
    {
      SpiBusMutex::Guard guard;
      hadExistingFile = Storage.exists(targetFilePath.c_str());
      if (hadExistingFile) {
        Storage.remove(targetFilePath.c_str());
      }
    }
    if (hadExistingFile) {
      LOG_DBG("WEB", "[%s] Overwriting existing file: %s", logLabel, targetFilePath.c_str());
    }

    esp_task_wdt_reset();
    bool opened = false;
    {
      SpiBusMutex::Guard guard;
      opened = Storage.openFileForWrite(storageTag, targetFilePath, uploadFile);
    }
    if (!opened) {
      uploadError = config.createFileError;
      LOG_DBG("WEB", "[%s] FAILED to create file: %s", logLabel, targetFilePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    LOG_DBG("WEB", "[%s] File created successfully: %s", logLabel, targetFilePath.c_str());
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = kBufferSize - uploadBufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(uploadBuffer + uploadBufferPos, data, toCopy);
        uploadBufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        if (uploadBufferPos >= kBufferSize) {
          if (!flushBuffer(logLabel)) {
            uploadError = config.chunkWriteError;
            {
              SpiBusMutex::Guard guard;
              uploadFile.close();
            }
            return;
          }
        }
      }

      uploadSize += upload.currentSize;

      if (config.logProgress && uploadSize - uploadLastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (uploadSize / 1024.0F) / (elapsed / 1000.0F) : 0.0F;
        LOG_DBG("WEB", "[%s] %u bytes (%.1f KB), %.1f KB/s, %u writes", logLabel, static_cast<unsigned int>(uploadSize),
                uploadSize / 1024.0F, kbps, static_cast<unsigned int>(writeCount));
        uploadLastLoggedSize = uploadSize;
      }
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      if (!flushBuffer(logLabel)) {
        uploadError = config.finalWriteError;
      }
      {
        SpiBusMutex::Guard guard;
        uploadFile.close();
      }

      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (uploadSize / 1024.0F) / (elapsed / 1000.0F) : 0.0F;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0F / elapsed) : 0.0F;
        LOG_DBG("WEB", "[%s] Complete: %s (%u bytes in %lu ms, avg %.1f KB/s)", logLabel, uploadFileName.c_str(),
                static_cast<unsigned int>(uploadSize), elapsed, avgKbps);
        LOG_DBG("WEB", "[%s] Diagnostics: %u writes, total write time: %lu ms (%.1f%%)", logLabel,
                static_cast<unsigned int>(writeCount), totalWriteTime, writePercent);
      }
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadBufferPos = 0;
    if (uploadFile) {
      SpiBusMutex::Guard guard;
      uploadFile.close();
      if (!targetFilePath.isEmpty()) {
        Storage.remove(targetFilePath.c_str());
      }
    }
    uploadError = config.abortedError;
    LOG_DBG("WEB", "[%s] Upload aborted", logLabel);
  }
}

void BufferedHttpUploadSession::reset() {
  if (uploadFile) {
    SpiBusMutex::Guard guard;
    uploadFile.close();
  }

  uploadFileName = "";
  uploadPathValue = "/";
  targetFilePath = "";
  uploadSize = 0;
  uploadSuccess = false;
  uploadError = "";
  uploadBufferPos = 0;
  uploadStartTime = 0;
  totalWriteTime = 0;
  writeCount = 0;
  uploadLastLoggedSize = 0;
}

}  // namespace network
