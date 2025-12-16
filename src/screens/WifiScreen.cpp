#include "WifiScreen.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include "CrossPointWebServer.h"
#include "config.h"

void WifiScreen::taskTrampoline(void* param) {
  auto* self = static_cast<WifiScreen*>(param);
  self->displayTaskLoop();
}

void WifiScreen::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  state = WifiScreenState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  keyboard.reset();

  // Trigger first update to show scanning message
  updateRequired = true;

  xTaskCreate(&WifiScreen::taskTrampoline, "WifiScreenTask",
              4096,               // Stack size (larger for WiFi operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Start WiFi scan
  startWifiScan();
}

void WifiScreen::onExit() {
  // Stop any ongoing WiFi scan
  WiFi.scanDelete();
  
  // Don't turn off WiFi if connected
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_OFF);
  }

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void WifiScreen::startWifiScan() {
  state = WifiScreenState::SCANNING;
  networks.clear();
  updateRequired = true;

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiScreen::processWifiScanResults() {
  int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    state = WifiScreenState::NETWORK_LIST;
    updateRequired = true;
    return;
  }

  // Scan complete, process results
  networks.clear();
  for (int i = 0; i < scanResult; i++) {
    WifiNetworkInfo network;
    network.ssid = WiFi.SSID(i).c_str();
    network.rssi = WiFi.RSSI(i);
    network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

    // Skip hidden networks (empty SSID)
    if (!network.ssid.empty()) {
      networks.push_back(network);
    }
  }

  // Sort by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(),
            [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) { return a.rssi > b.rssi; });

  WiFi.scanDelete();
  state = WifiScreenState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  updateRequired = true;
}

void WifiScreen::selectNetwork(int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];
  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;

  if (selectedRequiresPassword) {
    // Show password entry
    state = WifiScreenState::PASSWORD_ENTRY;
    keyboard.reset(new OnScreenKeyboard(
        renderer, inputManager,
        "Enter WiFi Password",
        "",    // No initial text
        64,    // Max password length
        false  // Show password by default (hard keyboard to use)
    ));
    updateRequired = true;
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiScreen::attemptConnection() {
  state = WifiScreenState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  updateRequired = true;

  WiFi.mode(WIFI_STA);
  
  if (selectedRequiresPassword && keyboard) {
    WiFi.begin(selectedSSID.c_str(), keyboard->getText().c_str());
  } else {
    WiFi.begin(selectedSSID.c_str());
  }
}

void WifiScreen::checkConnectionStatus() {
  if (state != WifiScreenState::CONNECTING) {
    return;
  }

  wl_status_t status = WiFi.status();
  
  if (status == WL_CONNECTED) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    state = WifiScreenState::CONNECTED;
    updateRequired = true;
    
    // Start the web server
    crossPointWebServer.begin();
    
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    connectionError = "Connection failed";
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = "Network not found";
    }
    state = WifiScreenState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }

  // Check for timeout
  if (millis() - connectionStartTime > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect();
    connectionError = "Connection timeout";
    state = WifiScreenState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }
}

void WifiScreen::handleInput() {
  // Check scan progress
  if (state == WifiScreenState::SCANNING) {
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiScreenState::CONNECTING) {
    checkConnectionStatus();
    return;
  }

  // Handle password entry state
  if (state == WifiScreenState::PASSWORD_ENTRY && keyboard) {
    keyboard->handleInput();
    
    if (keyboard->isComplete()) {
      attemptConnection();
      return;
    }
    
    if (keyboard->isCancelled()) {
      state = WifiScreenState::NETWORK_LIST;
      keyboard.reset();
      updateRequired = true;
      return;
    }
    
    updateRequired = true;
    return;
  }

  // Handle connected/failed states
  if (state == WifiScreenState::CONNECTED || state == WifiScreenState::CONNECTION_FAILED) {
    if (inputManager.wasPressed(InputManager::BTN_BACK) || 
        inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
      if (state == WifiScreenState::CONNECTION_FAILED) {
        // Go back to network list on failure
        state = WifiScreenState::NETWORK_LIST;
        updateRequired = true;
      } else {
        // Exit screen on success
        onGoBack();
      }
      return;
    }
  }

  // Handle network list state
  if (state == WifiScreenState::NETWORK_LIST) {
    // Check for Back button to exit
    if (inputManager.wasPressed(InputManager::BTN_BACK)) {
      onGoBack();
      return;
    }

    // Check for Confirm button to select network or rescan
    if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    // Handle UP/DOWN navigation
    if (inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT)) {
      if (selectedNetworkIndex > 0) {
        selectedNetworkIndex--;
        updateRequired = true;
      }
    } else if (inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT)) {
      if (!networks.empty() && selectedNetworkIndex < static_cast<int>(networks.size()) - 1) {
        selectedNetworkIndex++;
        updateRequired = true;
      }
    }
  }
}

std::string WifiScreen::getSignalStrengthIndicator(int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  } else if (rssi >= -60) {
    return "||| ";  // Good
  } else if (rssi >= -70) {
    return "||  ";  // Fair
  } else if (rssi >= -80) {
    return "|   ";  // Weak
  }
  return "    ";  // Very weak
}

void WifiScreen::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void WifiScreen::render() const {
  renderer.clearScreen();

  switch (state) {
    case WifiScreenState::SCANNING:
      renderConnecting();  // Reuse connecting screen with different message
      break;
    case WifiScreenState::NETWORK_LIST:
      renderNetworkList();
      break;
    case WifiScreenState::PASSWORD_ENTRY:
      renderPasswordEntry();
      break;
    case WifiScreenState::CONNECTING:
      renderConnecting();
      break;
    case WifiScreenState::CONNECTED:
      renderConnected();
      break;
    case WifiScreenState::CONNECTION_FAILED:
      renderConnectionFailed();
      break;
  }

  renderer.displayBuffer();
}

void WifiScreen::renderNetworkList() const {
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  // Draw header
  renderer.drawCenteredText(READER_FONT_ID, 10, "WiFi Networks", true, BOLD);

  if (networks.empty()) {
    // No networks found or scan failed
    const auto height = renderer.getLineHeight(UI_FONT_ID);
    const auto top = (pageHeight - height) / 2;
    renderer.drawCenteredText(UI_FONT_ID, top, "No networks found", true, REGULAR);
    renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, "Press OK to scan again", true, REGULAR);
  } else {
    // Calculate how many networks we can display
    const int startY = 60;
    const int lineHeight = 25;
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
        renderer.drawText(UI_FONT_ID, 5, networkY, ">");
      }

      // Draw network name (truncate if too long)
      std::string displayName = network.ssid;
      if (displayName.length() > 18) {
        displayName = displayName.substr(0, 15) + "...";
      }
      renderer.drawText(UI_FONT_ID, 20, networkY, displayName.c_str());

      // Draw signal strength indicator
      std::string signalStr = getSignalStrengthIndicator(network.rssi);
      renderer.drawText(UI_FONT_ID, pageWidth - 80, networkY, signalStr.c_str());

      // Draw lock icon for encrypted networks
      if (network.isEncrypted) {
        renderer.drawText(UI_FONT_ID, pageWidth - 30, networkY, "*");
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
    renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 45, countStr);
  }

  // Draw help text
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 30, "OK: Connect | BACK: Exit | * = Encrypted");
}

void WifiScreen::renderPasswordEntry() const {
  const auto pageHeight = GfxRenderer::getScreenHeight();

  // Draw header
  renderer.drawCenteredText(READER_FONT_ID, 5, "WiFi Password", true, BOLD);

  // Draw network name with good spacing from header
  std::string networkInfo = "Network: " + selectedSSID;
  if (networkInfo.length() > 30) {
    networkInfo = networkInfo.substr(0, 27) + "...";
  }
  renderer.drawCenteredText(UI_FONT_ID, 38, networkInfo.c_str(), true, REGULAR);

  // Draw keyboard
  if (keyboard) {
    keyboard->render(58);
  }
}

void WifiScreen::renderConnecting() const {
  const auto pageHeight = GfxRenderer::getScreenHeight();
  const auto height = renderer.getLineHeight(UI_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == WifiScreenState::SCANNING) {
    renderer.drawCenteredText(UI_FONT_ID, top, "Scanning...", true, REGULAR);
  } else {
    renderer.drawCenteredText(READER_FONT_ID, top - 30, "Connecting...", true, BOLD);
    
    std::string ssidInfo = "to " + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo = ssidInfo.substr(0, 22) + "...";
    }
    renderer.drawCenteredText(UI_FONT_ID, top, ssidInfo.c_str(), true, REGULAR);
  }
}

void WifiScreen::renderConnected() const {
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();
  const auto height = renderer.getLineHeight(UI_FONT_ID);
  const auto top = (pageHeight - height * 4) / 2;

  renderer.drawCenteredText(READER_FONT_ID, top - 30, "Connected!", true, BOLD);

  std::string ssidInfo = "Network: " + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo = ssidInfo.substr(0, 25) + "...";
  }
  renderer.drawCenteredText(UI_FONT_ID, top + 10, ssidInfo.c_str(), true, REGULAR);

  std::string ipInfo = "IP Address: " + connectedIP;
  renderer.drawCenteredText(UI_FONT_ID, top + 40, ipInfo.c_str(), true, REGULAR);

  // Show web server info
  std::string webInfo = "Web: http://" + connectedIP + "/";
  renderer.drawCenteredText(UI_FONT_ID, top + 70, webInfo.c_str(), true, REGULAR);

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, "Press any button to continue", true, REGULAR);
}

void WifiScreen::renderConnectionFailed() const {
  const auto pageHeight = GfxRenderer::getScreenHeight();
  const auto height = renderer.getLineHeight(UI_FONT_ID);
  const auto top = (pageHeight - height * 2) / 2;

  renderer.drawCenteredText(READER_FONT_ID, top - 20, "Connection Failed", true, BOLD);
  renderer.drawCenteredText(UI_FONT_ID, top + 20, connectionError.c_str(), true, REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, "Press any button to go back", true, REGULAR);
}
