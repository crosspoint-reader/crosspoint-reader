#pragma once

#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "WifiLinkWatch.h"
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
  // The row the mode picker opens on the first time (see ActivityManager::goToFileTransfer).
  NetworkMode initialSelection;
  bool isApMode = false;

  // Track whether the web server was started during this activity.
  bool webServerStarted = false;

  // Web server - owned by this activity
  std::unique_ptr<CrossPointWebServer> webServer;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name
  int currentRssi = 0;
  unsigned long lastRssiUpdateTime = 0;

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // STA mode: the link to the network. Gone (a drop that did not heal within its grace period)
  // ends the session, as Back does.
  WifiLinkWatch wifiWatch{"WEBACT"};

  // Set by the upload cancel check when Back is seen while an HTTP upload holds handleClient().
  // A flag rather than leaving it to the Back checks in loop(): the press the check saw may sit in
  // this tick's snapshot, which loop()'s own update() overwrites before it looks.
  bool leaveRequested = false;

  // Set after the first render completes and frame buffers are released.
  // Subsequent render() calls return immediately — no display operations
  // are possible after releaseFrameBuffers().
  bool buffersReleased = false;
  bool memoryFreedForRadio = false;

  void renderServerRunning() const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  // Unload SD fonts, paint the QR/URL screen, and release both frame buffers.
  // AP mode calls this before the WiFi stack starts so it gets the ~100KB of
  // headroom; STA mode calls it from startWebServer once the IP is known.
  // Frees what the radio needs (SD font, secondary buffer, glyph cache) — must run before any
  // WiFi bring-up on either path. Idempotent.
  void freeMemoryBeforeRadio();
  void showServerScreenAndReleaseBuffers();
  void startAccessPoint();
  void startWebServer();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       NetworkMode initialSelection = NetworkMode::JOIN_NETWORK)
      : Activity("CrossPointWebServer", renderer, mappedInput), initialSelection(initialSelection) {}
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
  // Suppress the minute-tick e-ink refresh while the web server is running.
  // That 640ms display cycle holds the render mutex and blocks handleClient.
  bool shouldSkipPeriodicUpdate() const override { return webServer && webServer->isRunning(); }
};
