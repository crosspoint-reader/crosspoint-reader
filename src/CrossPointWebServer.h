#pragma once

#include <WebServer.h>
#include <functional>
#include <string>
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
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  WebServer* server = nullptr;
  bool running = false;
  uint16_t port = 80;

  // File scanning
  std::vector<FileInfo> scanFiles(const char* path = "/");
  String formatFileSize(size_t bytes);
  bool isEpubFile(const String& filename);

  // Request handlers
  void handleRoot();
  void handleNotFound();
  void handleStatus();
  void handleFileList();
  void handleUpload();
  void handleUploadPost();
  void handleCreateFolder();
};

// Global instance
extern CrossPointWebServer crossPointWebServer;
