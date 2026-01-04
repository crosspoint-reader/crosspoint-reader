#pragma once

#include <memory>

// Must include SDCardManager before SimpleFTPServer to get SdFat
#include <SDCardManager.h>

// Configure SimpleFTPServer to use SdFat 2.x BEFORE including the library
#define STORAGE_TYPE_SDFAT2
#include <SimpleFTPServer.h>

class CrossPointFtpServer {
 public:
  CrossPointFtpServer();
  ~CrossPointFtpServer();

  // Start the FTP server (call after WiFi is connected)
  void begin();

  // Stop the FTP server
  void stop();

  // Call this periodically to handle client requests
  void handleClient() const;

  // Check if server is running
  bool isRunning() const { return running; }

  // Get the port number
  uint16_t getPort() const { return 21; }

 private:
  std::unique_ptr<::FtpServer> ftpServer;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
};
