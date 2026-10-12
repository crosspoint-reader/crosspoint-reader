#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;             // Whether we have saved credentials for this network
  std::string ipAddress;             // Populated after connection for display
  bool isHiddenPlaceholder = false;  // Synthetic "Add hidden network..." list entry
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  AUTO_CYCLING,       // Cycling through remaining saved credentials after AUTO_CONNECTING failed
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  HIDDEN_SSID_ENTRY,  // Entering SSID for a hidden network
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking whether to save the password (a ConfirmationActivity above)
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT,      // Offering to forget the network or reset its info (a ConfirmationActivity above)
  CAPTIVE_PORTAL      // Connected but network requires web-based login
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
  int selectedNetworkIndex = 0;
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

  // Whether we are attempting to auto-connect
  bool autoConnecting = false;
  // Whether this attempt has run WifiApChoice::improve(): once per attempt, so a reader that ends
  // up on the far AP again after a failed move connects rather than going round.
  bool apChoiceTried = false;

  // Saved-credential candidates for auto-cycling (SSIDs visible in scan, sorted by RSSI desc)
  std::vector<std::string> autoCycleCandidates;
  size_t autoCycleCandidateIndex = 0;
  bool autoCycleAfterScan = false;  // Scan was triggered to build cycle candidates

  // A network whose saved password failed like a wrong one this session. Picking it from the list
  // asks for the password instead of failing the same way again.
  std::string passwordSuspectSsid;

  // Connection timeouts
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  static constexpr unsigned long AUTO_CYCLE_TIMEOUT_MS = 5000;
  // Connect-time scan budget, applied via esp_wifi_set_scan_parameters() (see applyScanBudget()).
  // Scan duration is CHANNELS x DWELL and independent of SSID density; 80 ms puts a 13-channel
  // sweep at ~1040 ms against ~1560 ms at the IDF default of 120 ms, while keeping margin for an
  // AP that is slow to answer a probe. Too short lands back in NO_AP_FOUND plus a driver retry.
  static constexpr uint32_t SCAN_ACTIVE_DWELL_MAX_MS = 120;
  static constexpr uint32_t SCAN_PASSIVE_DWELL_MS = 200;
  // Documented minimum; only relevant while already associated, which this path is not.
  static constexpr uint8_t SCAN_HOME_CHAN_DWELL_MS = 30;

  unsigned long connectionStartTime = 0;

  // WiFi event handler IDs so we can deregister on exit.
  uint16_t evtIdConnected = 0;
  uint16_t evtIdGotIp = 0;
  uint16_t evtIdStaStart = 0;
  uint16_t evtIdDisconnected = 0;

  void renderNetworkList() const;
  void renderPasswordEntry() const;
  void renderConnecting() const;
  void renderConnected() const;
  void renderConnectionFailed() const;
  void renderCaptivePortal() const;

  void startWifiScan();
  void processWifiScanResults();
  void appendHiddenNetworkEntry();
  void buildAutoCycleCandidates();
  void tryNextAutoCycleCandidate();
  // True while the saved-network flow runs on its own (last-network attempt, the scan feeding the
  // cycle, the cycle itself) - the phases Back cancels and Confirm interrupts to show the list.
  bool isAutoConnectInProgress() const;
  void showNetworkListFromAutoConnect();
  void selectNetwork(int index);
  void promptHiddenSsid();
  // `title` replaces "Enter WiFi Password" when there is a reason to give.
  void promptPasswordEntry(const char* title = nullptr);
  void attemptConnection();
  void checkConnectionStatus();
  void abandonConnectAttempt();
  bool passwordLikelyWrong() const;
  bool askForPasswordIfLikelyWrong();
  bool promptForSuspectNetwork();
  // Issues WiFi.begin() with a full, signal-sorted scan. See the definition for why the cached
  // channel/BSSID hint that used to shortcut this was removed.
  void issueWifiBegin();
  // Bound the connect-time scan; see the definition.
  void applyScanBudget();
  // Prepares the WiFi stack for a connect attempt: ensures STA mode and a clean state,
  // sets a deterministic hostname. Skips the expensive disconnect(true,true) when WiFi
  // is already idle so the warm reconnect path doesn't pay an NVS-erase cost.
  void prepareForConnect();
  bool checkCaptivePortal();
  std::string getSignalStrengthIndicator(int32_t rssi) const;

  std::string captivePortalUrl;

  void askToSavePassword();
  void promptNetworkOptions();
  void onNetworkOptionsAnswer(const ActivityResult& result);
  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true)
      : Activity("WifiSelection", renderer, mappedInput), allowAutoConnect(autoConnect) {}
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  // Tap on a list row -> move the selection there; ActivityManager then synthesizes Confirm.
  ListRowTap::Result selectListRow(int index) override;
  // A swipe must not reach Left/Right, which are Options and Rescan here. Consumed without effect.
  bool pageList(ListPageDirection direction) override;
  void loop() override;
  void render(RenderLock&&) override;
  // Scanning and associating are work in flight, and a half-entered passphrase is
  // lost if the device sleeps out from under it. Callers reach this screen through
  // startActivityForResult(), so their own override stops applying while it is up.
  bool preventAutoSleep() override { return true; }
};
