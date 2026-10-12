#pragma once

#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "ServerSession.h"
#include "activities/Activity.h"

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

  // The server, its start, its requests, its link and its end (ServerSession.h). STA mode: a link
  // that is Gone (a drop that did not heal within its grace period) ends the session, as Back does.
  ServerSession session{renderer, mappedInput, "WEBACT"};

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name
  int currentRssi = 0;
  unsigned long lastRssiUpdateTime = 0;

  // Set after the first render completes and frame buffers are released.
  // Subsequent render() calls return immediately — no display operations
  // are possible after releaseFrameBuffers().
  bool buffersReleased = false;

  void renderServerRunning() const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  // Unload SD fonts, paint the QR/URL screen, and release both frame buffers.
  // AP mode calls this before the WiFi stack starts so it gets the ~100KB of
  // headroom; STA mode calls it from startWebServer once the IP is known.
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
  bool skipLoopDelay() override { return session.running(); }
  bool preventAutoSleep() override { return session.running(); }
  // Suppress the minute-tick e-ink refresh while the web server is running.
  // That 640ms display cycle holds the render mutex and blocks handleClient.
  bool shouldSkipPeriodicUpdate() const override { return session.running(); }
};
