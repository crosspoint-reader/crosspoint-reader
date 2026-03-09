#pragma once

#include <HalStorage.h>
#include <WebServer.h>

#include <cstddef>

namespace network {

struct BufferedHttpUploadConfig {
  const char* logLabel = "UPLOAD";
  const char* storageTag = nullptr;
  const char* createFileError = "Failed to create file on SD card";
  const char* chunkWriteError = "Failed to write upload data";
  const char* finalWriteError = "Failed to write final upload data";
  const char* abortedError = "Upload aborted";
  bool logProgress = false;
  bool (*resolveTarget)(WebServer* server, const char* fileName, char* uploadPath, size_t uploadPathSize,
                        char* filePath, size_t filePathSize, char* error, size_t errorSize) = nullptr;
};

class BufferedHttpUploadSession {
 public:
  static constexpr size_t kBufferSize = 4096;
  static constexpr size_t kMaxFileNameLen = 256;
  static constexpr size_t kMaxUploadPathLen = 256;
  static constexpr size_t kMaxTargetFilePathLen = 512;
  static constexpr size_t kMaxErrorLen = 128;

  void handleUpload(WebServer* server, const BufferedHttpUploadConfig& config);
  void reset();

  bool succeeded() const { return uploadSuccess; }
  const char* fileName() const { return uploadFileName; }
  const char* uploadPath() const { return uploadPathValue; }
  const char* filePath() const { return targetFilePath; }
  const char* error() const { return uploadError; }
  size_t size() const { return uploadSize; }

 private:
  bool flushBuffer(const char* logLabel);

  FsFile uploadFile;
  char uploadFileName[kMaxFileNameLen] = {};
  char uploadPathValue[kMaxUploadPathLen] = "/";
  char targetFilePath[kMaxTargetFilePathLen] = {};
  size_t uploadSize = 0;
  bool uploadSuccess = false;
  char uploadError[kMaxErrorLen] = {};
  uint8_t uploadBuffer[kBufferSize] = {};
  size_t uploadBufferPos = 0;
  unsigned long uploadStartTime = 0;
  unsigned long totalWriteTime = 0;
  size_t writeCount = 0;
  size_t uploadLastLoggedSize = 0;
};

BufferedHttpUploadSession& sharedBufferedHttpUploadSession();

}  // namespace network
