#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "OnScreenKeyboard.h"

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;  // Whether we have saved credentials for this network
};

// WiFi screen states
enum class WifiScreenState {
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected, showing IP
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT       // Asking user if they want to forget the network
};

class WifiScreen final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  WifiScreenState state = WifiScreenState::SCANNING;
  int selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  const std::function<void()> onGoBack;

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // On-screen keyboard for password entry
  std::unique_ptr<OnScreenKeyboard> keyboard;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Connection timeout
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  unsigned long connectionStartTime = 0;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderNetworkList() const;
  void renderPasswordEntry() const;
  void renderConnecting() const;
  void renderConnected() const;
  void renderSavePrompt() const;
  void renderConnectionFailed() const;
  void renderForgetPrompt() const;

  void startWifiScan();
  void processWifiScanResults();
  void selectNetwork(int index);
  void attemptConnection();
  void checkConnectionStatus();
  std::string getSignalStrengthIndicator(int32_t rssi) const;

 public:
  explicit WifiScreen(GfxRenderer& renderer, InputManager& inputManager, const std::function<void()>& onGoBack)
      : Activity(renderer, inputManager), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
