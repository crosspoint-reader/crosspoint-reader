#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;
struct WifiCredential;

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;             // Whether we have saved credentials for this network
  bool isHiddenPlaceholder = false;  // Synthetic "Add hidden network..." list entry
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  HIDDEN_SSID_ENTRY,  // Entering SSID for a hidden network
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT       // Asking user if they want to forget the network
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Save the password if requested
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  WifiSelectionState state = WifiSelectionState::SCANNING;
  size_t selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  // Number of real (scanned) networks, excluding the synthetic hidden-network entry
  size_t realNetworkCount = 0;

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Whether to attempt auto-connect on entry
  const bool allowAutoConnect;

  // Whether we are attempting to auto-connect or auto-scan saved networks.
  bool autoConnecting = false;

  // True when the user stopped auto-connect and asked to see the scan result.
  bool manualNetworkListRequested = false;

  // Saved SSIDs already attempted during the current auto-connect session.
  std::vector<std::string> autoAttemptedSsids;

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Connection timeout
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  static constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 7000;
  unsigned long connectionStartTime = 0;

  void renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderPasswordEntry(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnected(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderSavePrompt(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const;

  void startWifiScan(bool autoScan = false);
  void processWifiScanResults();
  void appendHiddenNetworkEntry();
  void selectNetwork(int index);
  void promptHiddenSsid();
  void promptPasswordEntry();
  void attemptConnection();
  void checkConnectionStatus();
  bool tryAutoConnectCredential(const WifiCredential& cred);
  bool tryNextSavedNetworkFromScan();
  void handleAutoConnectFailure();
  void showNetworkListFromAutoConnect();
  bool hasAttemptedAutoSsid(const std::string& ssid) const;
  std::string getSignalStrengthIndicator(int32_t rssi) const;

  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true)
      : Activity("WifiSelection", renderer, mappedInput), allowAutoConnect(autoConnect) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
