#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "ProtocolSelectionActivity.h"
#include "activities/ActivityWithSubactivity.h"
#include "network/CrossPointWebServer.h"
#include "network/CrossPointFtpServer.h"

// File transfer activity states
enum class FileTransferActivityState {
  MODE_SELECTION,      // Choosing between Join Network and Create Hotspot
  PROTOCOL_SELECTION,  // Choosing between HTTP and FTP
  WIFI_SELECTION,      // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,         // Starting Access Point mode
  SERVER_RUNNING,      // File transfer server is running and handling requests
  SHUTTING_DOWN        // Shutting down server and WiFi
};

/**
 * FileTransferActivity is the entry point for file transfer functionality.
 * It:
 * - First presents a choice between "Join a Network" (STA) and "Create Hotspot" (AP)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the file transfer server (HTTP or FTP) when connected
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class FileTransferActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  FileTransferActivityState state = FileTransferActivityState::MODE_SELECTION;
  const std::function<void()> onGoBack;

  // Network mode
  NetworkMode networkMode = NetworkMode::JOIN_NETWORK;
  bool isApMode = false;

  // Transfer protocol
  FileTransferProtocol selectedProtocol = FileTransferProtocol::HTTP;

  // File transfer servers - owned by this activity
  std::unique_ptr<CrossPointWebServer> httpServer;
  std::unique_ptr<CrossPointFtpServer> ftpServer;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // Auto-shutdown tracking
  unsigned long serverStartTime = 0;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderServerRunning() const;
  void renderHttpServerRunning() const;
  void renderFtpServerRunning() const;

  void onNetworkModeSelected(NetworkMode mode);
  void onProtocolSelected(FileTransferProtocol protocol);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startServer();
  void stopHttpServer();
  void stopFtpServer();

 public:
  explicit FileTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("FileTransfer", renderer, mappedInput), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override {
    return (httpServer && httpServer->isRunning()) || (ftpServer && ftpServer->isRunning());
  }
};
