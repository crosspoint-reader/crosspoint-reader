#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include <map>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WifiSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<WifiSelectionActivity*>(param);
  self->displayTaskLoop();
}

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  WIFI_STORE.loadFromFile();
  xSemaphoreGive(renderingMutex);

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;

  // Cache MAC address for display
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[32];
  snprintf(macStr, sizeof(macStr), "MAC address: %02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
  cachedMacAddress = std::string(macStr);

  // Task creation
  xTaskCreate(&WifiSelectionActivity::taskTrampoline, "WifiSelectionTask",
              4096,               // Stack size (larger for WiFi operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Attempt to auto-connect to the last network
  if (allowAutoConnect) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto* cred = WIFI_STORE.findCredential(lastSsid);
      if (cred) {
        LOG_DBG("WIFI", "Attempting to auto-connect to %s", lastSsid.c_str());
        selectedSSID = cred->ssid;
        enteredPassword = cred->password;
        selectedRequiresPassword = !cred->password.empty();
        usedSavedPassword = true;
        autoConnecting = true;
        attemptConnection();
        updateRequired = true;
        return;
      }
    }
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WIFI] [MEM", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI] [MEM", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  // Acquire mutex before deleting task to ensure task isn't using it
  // This prevents hangs/crashes if the task holds the mutex when deleted
  LOG_DBG("WIFI", "Acquiring rendering mutex before task deletion...");
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task (we now hold the mutex, so task is blocked if it
  // needs it)
  LOG_DBG("WIFI", "Deleting display task...");
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    LOG_DBG("WIFI", "Display task deleted");
  }

  // Now safe to delete the mutex since we own it
  LOG_DBG("WIFI", "Deleting mutex...");
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  LOG_DBG("WIFI", "Mutex deleted");

  LOG_DBG("WIFI] [MEM", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan() {
  autoConnecting = false;
  state = WifiSelectionState::SCANNING;
  networks.clear();
  updateRequired = true;

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    state = WifiSelectionState::NETWORK_LIST;
    updateRequired = true;
    return;
  }

  // Scan complete, process results
  // Use a map to deduplicate networks by SSID, keeping the strongest signal
  std::map<std::string, WifiNetworkInfo> uniqueNetworks;

  for (int i = 0; i < scanResult; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    const int32_t rssi = WiFi.RSSI(i);

    // Skip hidden networks (empty SSID)
    if (ssid.empty()) {
      continue;
    }

    // Check if we've already seen this SSID
    auto it = uniqueNetworks.find(ssid);
    if (it == uniqueNetworks.end() || rssi > it->second.rssi) {
      // New network or stronger signal than existing entry
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      uniqueNetworks[ssid] = network;
    }
  }

  // Convert map to vector
  networks.clear();
  for (const auto& pair : uniqueNetworks) {
    // cppcheck-suppress useStlAlgorithm
    networks.push_back(pair.second);
  }

  // Sort by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(),
            [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) { return a.rssi > b.rssi; });

  // Show networks with PW first
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    return a.hasSavedPassword && !b.hasSavedPassword;
  });

  WiFi.scanDelete();
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  updateRequired = true;
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];
  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Check if we have saved credentials for this network
  const auto* savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_DBG("WiFi", "Using saved password for %s, length: %zu", selectedSSID.c_str(), enteredPassword.size());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    // Show password entry
    state = WifiSelectionState::PASSWORD_ENTRY;
    // Don't allow screen updates while changing activity
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Enter WiFi Password",
        "",     // No initial text
        50,     // Y position
        64,     // Max password length
        false,  // Show password by default (hard keyboard to use)
        [this](const std::string& text) {
          enteredPassword = text;
          exitActivity();
        },
        [this] {
          state = WifiSelectionState::NETWORK_LIST;
          updateRequired = true;
          exitActivity();
        }));
    updateRequired = true;
    xSemaphoreGive(renderingMutex);
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::attemptConnection() {
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  updateRequired = true;

  WiFi.mode(WIFI_STA);

  if (selectedRequiresPassword && !enteredPassword.empty()) {
    WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    WiFi.begin(selectedSSID.c_str());
  }
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    autoConnecting = false;

    // Save this as the last connected network - SD card operations need lock as
    // we use SPI for both
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    WIFI_STORE.setLastConnectedSsid(selectedSSID);
    xSemaphoreGive(renderingMutex);

    // If we entered a new password, ask if user wants to save it
    // Otherwise, immediately complete so parent can start web server
    if (!usedSavedPassword && !enteredPassword.empty()) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      updateRequired = true;
    } else {
      // Using saved password or open network - complete immediately
      LOG_DBG("WIFI",
              "Connected with saved/open credentials, "
              "completing immediately");
      onComplete(true);
    }
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    connectionError = "Error: General failure";
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = "Error: Network not found";
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }

  // Check for timeout
  if (millis() - connectionStartTime > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect();
    connectionError = "Error: Connection timeout";
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }
}

void WifiSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Check scan progress
  if (state == WifiSelectionState::SCANNING) {
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    checkConnectionStatus();
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
        xSemaphoreGive(renderingMutex);
      }
      // Complete - parent will start web server
      onComplete(true);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      onComplete(true);
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        // User chose "Forget network" - forget the network
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        WIFI_STORE.removeCredential(selectedSSID);
        xSemaphoreGive(renderingMutex);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was selected)
      startWifiScan();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      startWifiScan();
    }
    return;
  }

  // Handle connected state (should not normally be reached - connection
  // completes immediately)
  if (state == WifiSelectionState::CONNECTED) {
    // Safety fallback - immediately complete
    onComplete(true);
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // If we were auto-connecting or using a saved credential, offer to forget
      // the network
      if (autoConnecting || usedSavedPassword) {
        autoConnecting = false;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
      } else {
        // Go back to network list on failure for non-saved credentials
        state = WifiSelectionState::NETWORK_LIST;
      }
      updateRequired = true;
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        updateRequired = true;
        return;
      }
    }

    // Handle navigation
    buttonNavigator.onNext([this] {
      selectedNetworkIndex = ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size());
      updateRequired = true;
    });

    buttonNavigator.onPrevious([this] {
      selectedNetworkIndex = ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size());
      updateRequired = true;
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return "||| ";  // Good
  }
  if (rssi >= -70) {
    return "||  ";  // Fair
  }
  if (rssi >= -80) {
    return "|   ";  // Weak
  }
  return "    ";  // Very weak
}

void WifiSelectionActivity::displayTaskLoop() {
  while (true) {
    // If a subactivity is active, yield CPU time but don't render
    if (subActivity) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    // Don't render if we're in PASSWORD_ENTRY state - we're just transitioning
    // from the keyboard subactivity back to the main activity
    if (state == WifiSelectionState::PASSWORD_ENTRY) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void WifiSelectionActivity::render() const {
  renderer.clearScreen();

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting();
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting();  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList();
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting();
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected();
      break;
    case WifiSelectionState::SAVE_PROMPT:
      renderSavePrompt();
      break;
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed();
      break;
    case WifiSelectionState::FORGET_PROMPT:
      renderForgetPrompt();
      break;
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "WiFi Networks", true, EpdFontFamily::BOLD);

  if (networks.empty()) {
    // No networks found or scan failed
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = (pageHeight - height) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, "No networks found");
    renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, "Press Connect to scan again");
  } else {
    // Calculate how many networks we can display
    constexpr int startY = 60;
    constexpr int lineHeight = 25;
    const int maxVisibleNetworks = (pageHeight - startY - 40) / lineHeight;

    // Calculate scroll offset to keep selected item visible
    int scrollOffset = 0;
    if (selectedNetworkIndex >= maxVisibleNetworks) {
      scrollOffset = selectedNetworkIndex - maxVisibleNetworks + 1;
    }

    // Draw networks
    int displayIndex = 0;
    for (size_t i = scrollOffset; i < networks.size() && displayIndex < maxVisibleNetworks; i++, displayIndex++) {
      const int networkY = startY + displayIndex * lineHeight;
      const auto& network = networks[i];

      // Draw selection indicator
      if (static_cast<int>(i) == selectedNetworkIndex) {
        renderer.drawText(UI_10_FONT_ID, 5, networkY, ">");
      }

      // Draw network name (truncate if too long)
      std::string displayName = network.ssid;
      if (displayName.length() > 33) {
        displayName.replace(30, displayName.length() - 30, "...");
      }
      renderer.drawText(UI_10_FONT_ID, 20, networkY, displayName.c_str());

      // Draw signal strength indicator
      std::string signalStr = getSignalStrengthIndicator(network.rssi);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 90, networkY, signalStr.c_str());

      // Draw saved indicator (checkmark) for networks with saved passwords
      if (network.hasSavedPassword) {
        renderer.drawText(UI_10_FONT_ID, pageWidth - 50, networkY, "+");
      }

      // Draw lock icon for encrypted networks
      if (network.isEncrypted) {
        renderer.drawText(UI_10_FONT_ID, pageWidth - 30, networkY, "*");
      }
    }

    // Draw scroll indicators if needed
    if (scrollOffset > 0) {
      renderer.drawText(SMALL_FONT_ID, pageWidth - 15, startY - 10, "^");
    }
    if (scrollOffset + maxVisibleNetworks < static_cast<int>(networks.size())) {
      renderer.drawText(SMALL_FONT_ID, pageWidth - 15, startY + maxVisibleNetworks * lineHeight, "v");
    }

    // Show network count
    char countStr[32];
    snprintf(countStr, sizeof(countStr), "%zu networks found", networks.size());
    renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 90, countStr);
  }

  // Show MAC address above the network count and legend
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 105, cachedMacAddress.c_str());

  // Draw help text
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 75, "* = Encrypted | + = Saved");

  const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? "Forget" : "";

  const auto labels = mappedInput.mapLabels("« Back", "Connect", forgetLabel, "Refresh");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == WifiSelectionState::SCANNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Scanning...");
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, top - 40, "Connecting...", true, EpdFontFamily::BOLD);

    std::string ssidInfo = "to " + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo.replace(22, ssidInfo.length() - 22, "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, top, ssidInfo.c_str());
  }
}

void WifiSelectionActivity::renderConnected() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 4) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 30, "Connected!", true, EpdFontFamily::BOLD);

  std::string ssidInfo = "Network: " + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = "IP Address: " + connectedIP;
  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", "Continue", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderSavePrompt() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, "Connected!", true, EpdFontFamily::BOLD);

  std::string ssidInfo = "Network: " + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top, ssidInfo.c_str());

  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, "Save password for next time?");

  // Draw Yes/No buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  // Draw "Yes" button
  if (savePromptSelection == 0) {
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, "[Yes]");
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, "Yes");
  }

  // Draw "No" button
  if (savePromptSelection == 1) {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, "[No]");
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, "No");
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("« Skip", "Select", "Left", "Right");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 2) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 20, "Connection Failed", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("« Back", "Continue", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderForgetPrompt() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, "Forget Network", true, EpdFontFamily::BOLD);
  std::string ssidInfo = "Network: " + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top, ssidInfo.c_str());

  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, "Forget network and remove saved password?");

  // Draw Cancel/Forget network buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  // Draw "Cancel" button
  if (forgetPromptSelection == 0) {
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, "[Cancel]");
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, "Cancel");
  }

  // Draw "Forget network" button
  if (forgetPromptSelection == 1) {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, "[Forget network]");
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, "Forget network");
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Left", "Right");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
