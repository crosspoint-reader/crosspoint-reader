#pragma once

#include <FeatureFlags.h>
#include <HalStorage.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#if __has_include(<NetworkUdp.h>)
#include <NetworkUdp.h>
#define CROSSPOINT_HAS_NETWORKUDP 1
using CrossPointUdpType = NetworkUDP;
#else
#include <WiFiUdp.h>
#define CROSSPOINT_HAS_NETWORKUDP 0
using CrossPointUdpType = WiFiUDP;
#endif

#include <cstddef>
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

  // Count of meaningful API requests (status checks, uploads, downloads)
  uint32_t getRequestCount() const { return requestCount; }

  // Returns true if any push/pull API request was received since the server started
  bool hadApiActivity() const { return requestCount > 0; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;               // WebSocket port
  mutable uint32_t requestCount = 0;  // Incremented on status/upload/download
  CrossPointUdpType udp;
  bool udpActive = false;

  // WebSocket upload state
  FsFile wsUploadFile;
  String wsUploadFileName;
  String wsUploadPath;
  size_t wsUploadSize = 0;
  size_t wsUploadReceived = 0;
  size_t wsLastProgressSent = 0;
  unsigned long wsUploadStartTime = 0;
  bool wsUploadInProgress = false;
  uint8_t wsUploadOwnerClient = 0;
  bool wsUploadOwnerValid = false;
  String wsLastCompleteName;
  size_t wsLastCompleteSize = 0;
  unsigned long wsLastCompleteAt = 0;

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
  void handlePlugins() const;
  void handleTodoEntry();
  void handleTodoTodayGet() const;
  void handleTodoTodaySave() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload();
  void handleUploadPost();
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // API handlers for web UI
  void handleRecentBooks() const;
  void handleGetBookProgress() const;
  void handleCover() const;
  void handleSleepImages() const;
  void handleSleepCoverGet() const;
  void handleSleepCoverPin();

  // Remote control
  void handleOpenBook();
  void handleRemoteButton();
  void handleScreenshot();
  void handleGetSettingsRaw() const;
};
