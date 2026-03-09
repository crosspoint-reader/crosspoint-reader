#pragma once

#include <HalStorage.h>
#include <WebServer.h>

#include <cstddef>

namespace network {

struct BufferedHttpUploadTarget {
  String uploadPath = "/";
  String filePath;
};

struct BufferedHttpUploadConfig {
  const char* logLabel = "UPLOAD";
  const char* storageTag = nullptr;
  const char* createFileError = "Failed to create file on SD card";
  const char* chunkWriteError = "Failed to write upload data";
  const char* finalWriteError = "Failed to write final upload data";
  const char* abortedError = "Upload aborted";
  bool logProgress = false;
  bool (*resolveTarget)(WebServer* server, const String& fileName, BufferedHttpUploadTarget& target,
                        String& error) = nullptr;
};

class BufferedHttpUploadSession {
 public:
  static constexpr size_t kBufferSize = 4096;

  void handleUpload(WebServer* server, const BufferedHttpUploadConfig& config);
  void reset();

  bool succeeded() const { return uploadSuccess; }
  const String& fileName() const { return uploadFileName; }
  const String& uploadPath() const { return uploadPathValue; }
  const String& filePath() const { return targetFilePath; }
  const String& error() const { return uploadError; }
  size_t size() const { return uploadSize; }

 private:
  bool flushBuffer(const char* logLabel);

  FsFile uploadFile;
  String uploadFileName;
  String uploadPathValue = "/";
  String targetFilePath;
  size_t uploadSize = 0;
  bool uploadSuccess = false;
  String uploadError;
  uint8_t uploadBuffer[kBufferSize] = {};
  size_t uploadBufferPos = 0;
  unsigned long uploadStartTime = 0;
  unsigned long totalWriteTime = 0;
  size_t writeCount = 0;
  size_t uploadLastLoggedSize = 0;
};

BufferedHttpUploadSession& sharedBufferedHttpUploadSession();

}  // namespace network
