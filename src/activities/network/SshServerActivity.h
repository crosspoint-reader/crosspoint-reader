#pragma once

#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "activities/Activity.h"
#include "network/SshServer.h"

// SSH server activity states
enum class SshServerActivityState {
  MODE_SELECTION,  // Choosing between Join Network and Create Hotspot
  WIFI_SELECTION,  // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,     // Starting Access Point mode
  SERVER_RUNNING,  // SSH server is running and handling sessions
  SHUTTING_DOWN    // Shutting down server and WiFi
};

/**
 * SshServerActivity is the entry point for file transfer / remote access.
 * It:
 * - First presents a choice between "Join a Network" (STA) and "Create Hotspot" (AP)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the SshServer when connected and shows the connection details
 *   (ssh command plus a per-session generated password)
 * - Cleans up the server and shuts down WiFi on exit
 */
class SshServerActivity final : public Activity {
  SshServerActivityState state = SshServerActivityState::MODE_SELECTION;

  bool isApMode = false;

  // SSH server - owned by this activity
  std::unique_ptr<SshServer> sshServer;

  // Per-session password shown on screen
  char sessionPassword[12] = {0};

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Last observed transfer status, for change detection
  SshServer::TransferStatus lastStatus;
  unsigned long lastStatusPollAt = 0;
  unsigned long lastProgressRepaintAt = 0;
  static constexpr unsigned long PROGRESS_REPAINT_MS = 1000;

  // Sustained WiFi-loss tracking; abandon only after WIFI_ABANDON_MS.
  int consecutiveDisconnects = 0;
  unsigned long firstDisconnectAt = 0;
  static constexpr unsigned long WIFI_ABANDON_MS = 5UL * 60UL * 1000UL;

  // Cached signal-strength bracket (0..4) for the WiFi indicator.
  int lastWifiBars = 0;

  void renderServerRunning() const;
  void renderWifiIndicator(int subHeaderTop) const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void launchModeSelection();
  void startAccessPoint();
  void startSshServer();
  void generatePassword();
  void pollServerStatus();
  void monitorWifi();

 public:
  explicit SshServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SshServer", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return sshServer && sshServer->isRunning(); }
};
