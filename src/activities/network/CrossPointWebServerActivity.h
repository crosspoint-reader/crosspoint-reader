#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "NetworkModeSelectionActivity.h"
#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"

// Web server activity states
enum class WebServerActivityState {
  MODE_SELECTION,  // Choosing between Join Network and Create Hotspot
  WIFI_SELECTION,  // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,     // Starting Access Point mode
  SERVER_RUNNING,  // Web server is running and handling requests
  SHUTTING_DOWN    // Shutting down server and WiFi
};

/**
 * CrossPointWebServerActivity is the entry point for file transfer functionality.
 * It:
 * - First presents a choice between "Join a Network" (STA), "Connect to Calibre", and "Create Hotspot" (AP)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the CrossPointWebServer when connected
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class CrossPointWebServerActivity final : public Activity {
  WebServerActivityState state = WebServerActivityState::MODE_SELECTION;

  // Network mode
  NetworkMode networkMode = NetworkMode::JOIN_NETWORK;
  bool isApMode = false;

  // Web server - owned by this activity
  std::unique_ptr<CrossPointWebServer> webServer;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // Transfer status tracking (for display updates)
  bool lastUploadInProgress = false;
  size_t lastUploadReceived = 0;
  unsigned long lastTransferUpdateTime = 0;
  unsigned long lastKnownCompleteAt = 0;
  std::vector<std::string> uploadedFiles;
  int lastReceivedFileCount =
      0;  // tracks UITheme list for live feed updates  // Accumulated list of completed uploads (oldest first)

  void renderServerRunning() const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startWebServer();
  void stopWebServer();

  // When true, onEnter() skips mode/WiFi selection and starts the server immediately
  // using the already-established WiFi connection (e.g. after DZ auto-connect on boot).
  bool preConnected = false;

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       bool preConnected = false)
      : Activity("CrossPointWebServer", renderer, mappedInput), preConnected(preConnected) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
