#pragma once

#include <HalStorage.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFiUdp.h>

#include <memory>
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
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    FsFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    UploadState() { buffer.resize(UPLOAD_BUFFER_SIZE); }
  } upload;

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

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  WiFiUDP udp;
  bool udpActive = false;

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

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
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;
};
