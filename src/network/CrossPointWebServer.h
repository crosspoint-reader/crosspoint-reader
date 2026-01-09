#pragma once

#include <SdFat.h>
#include <WebServer.h>

#include <atomic>
#include <mutex>
#include <vector>

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient() const;

  // Check if server is running
  bool isRunning() const { return running.load(std::memory_order_acquire); }

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::atomic<bool> running{false};
  mutable std::mutex serverMutex;  // Protects server pointer access
  bool apMode = false;             // true when running in AP mode, false for STA mode
  uint16_t port = 80;

  // Upload state (instance variables with mutex protection)
  mutable std::mutex uploadMutex;
  mutable FsFile uploadFile;
  mutable String uploadFileName;
  mutable String uploadPath;
  mutable size_t uploadSize;
  mutable bool uploadSuccess;
  mutable String uploadError;
  mutable unsigned long lastWriteTime;
  mutable unsigned long uploadStartTime;
  mutable size_t lastLoggedSize;

  // File scanning
  void scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleUpload() const;
  void handleUploadPost() const;
  void handleCreateFolder() const;
  void handleDelete() const;
};
