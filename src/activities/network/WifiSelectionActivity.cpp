#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <map>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

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
  isManualSsid = false;

  // Cache MAC address for display
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[64];
  snprintf(macStr, sizeof(macStr), "%s %02x-%02x-%02x-%02x-%02x-%02x", tr(STR_MAC_ADDRESS), mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  cachedMacAddress = std::string(macStr);

  // Trigger first update to show scanning message
  requestUpdate();

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
        requestUpdate();
        return;
      }
    }
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan() {
  autoConnecting = false;
  isManualSsid = false;
  selectedNetworkIndex = 0;
  state = WifiSelectionState::SCANNING;
  networks.clear();
  requestUpdate();

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
    requestUpdate();
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

  // For credentials explicitly marked as hidden networks, do a targeted
  // synchronous scan per SSID. Hidden APs don't broadcast their SSID in
  // beacon frames, so they won't appear in the regular async scan above.
  // The targeted scan sends directed probe requests so the AP can respond.
  const auto& savedCreds = WIFI_STORE.getCredentials();
  for (const auto& cred : savedCreds) {
    if (!cred.isHidden) {
      continue;  // Non-hidden saved networks are covered by the regular scan
    }
    if (uniqueNetworks.find(cred.ssid) != uniqueNetworks.end()) {
      continue;  // Already found in the regular scan (e.g., network was made visible)
    }

    LOG_DBG("WIFI", "Targeted scan for hidden SSID: %s", cred.ssid.c_str());
    const int16_t targetResult = WiFi.scanNetworks(false,             // async = false (blocking)
                                                   true,              // show_hidden = true
                                                   false,             // passive = false (active scan)
                                                   300,               // timeout per channel in ms
                                                   0,                 // all channels
                                                   cred.ssid.c_str()  // directed probe to this SSID
    );

    WifiNetworkInfo hidden;
    hidden.ssid = cred.ssid;
    hidden.hasSavedPassword = true;
    hidden.isHidden = true;

    if (targetResult > 0) {
      // AP responded - use actual RSSI and encryption type from the scan result
      hidden.rssi = WiFi.RSSI(0);
      hidden.isEncrypted = (WiFi.encryptionType(0) != WIFI_AUTH_OPEN);
      LOG_DBG("WIFI", "Hidden SSID %s found in range, RSSI=%d, encrypted=%s", cred.ssid.c_str(), hidden.rssi,
              hidden.isEncrypted ? "true" : "false");
    } else {
      // Not found - fall back to a password-based encryption guess since we have no scan data
      hidden.rssi = -101;
      hidden.isEncrypted = !cred.password.empty();
      LOG_DBG("WIFI", "Hidden SSID %s not found in targeted scan", cred.ssid.c_str());
    }
    WiFi.scanDelete();

    uniqueNetworks[cred.ssid] = hidden;
  }

  // Convert map to vector
  networks.clear();
  for (const auto& pair : uniqueNetworks) {
    // cppcheck-suppress useStlAlgorithm
    networks.push_back(pair.second);
  }

  // Sort: saved-password networks first, then hidden networks within saved,
  // then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    // Among saved networks: visible before hidden
    if (a.hasSavedPassword && b.hasSavedPassword && a.isHidden != b.isHidden) {
      return !a.isHidden;
    }
    return a.rssi > b.rssi;
  });

  WiFi.scanDelete();
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
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
  isManualSsid = network.isHidden;

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
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
        "",     // No initial text
        64,     // Max password length
        false,  // Show password by default (hard keyboard to use)
        [this](const std::string& text) {
          enteredPassword = text;
          exitActivity();
        },
        [this] {
          state = WifiSelectionState::NETWORK_LIST;
          exitActivity();
          requestUpdate();
        }));
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
  requestUpdate();

  WiFi.mode(WIFI_STA);

  // Set hostname so routers show "CrossPoint-Reader-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());

  // For manually entered SSIDs and saved credentials marked as hidden, perform
  // a targeted scan first. This sends directed probe requests to the specific
  // SSID, which is required to discover hidden networks (they don't broadcast
  // their SSID in beacon frames). Without this, WiFi.begin() may immediately
  // return WL_NO_SSID_AVAIL for hidden networks.
  const auto* targetedScanCred = WIFI_STORE.findCredential(selectedSSID);
  const bool needsTargetedScan = isManualSsid || (targetedScanCred && targetedScanCred->isHidden);
  if (needsTargetedScan) {
    LOG_DBG("WIFI", "Doing targeted scan for SSID: %s", selectedSSID.c_str());
    WiFi.scanNetworks(false,                // async = false (blocking)
                      true,                 // show_hidden = true
                      false,                // passive = false (active scan with directed probes)
                      300,                  // timeout per channel in ms
                      0,                    // channel = 0 (scan all channels)
                      selectedSSID.c_str()  // target SSID for directed probe requests
    );
    WiFi.scanDelete();
    LOG_DBG("WIFI", "Targeted scan complete");
  }

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
    {
      RenderLock lock(*this);
      WIFI_STORE.setLastConnectedSsid(selectedSSID);
    }

    // If we entered a new password, ask if user wants to save it
    // Otherwise, immediately complete so parent can start web server
    // Also trigger save prompt for manually entered hidden networks even when
    // the password is empty, so the SSID is persisted with isHidden = true.
    if (!usedSavedPassword && (!enteredPassword.empty() || isManualSsid)) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      requestUpdate();
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
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }

  // Check for timeout
  if (millis() - connectionStartTime > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect();
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
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

  if (state == WifiSelectionState::SSID_ENTRY) {
    // Reached here after SSID keyboard subactivity exits.
    if (selectedSSID.empty()) {
      // Cancelled or empty - go back to network list
      state = WifiSelectionState::NETWORK_LIST;
      requestUpdate();
      return;
    }
    // SSID entered - now ask for password (always prompt so user can leave
    // blank for open networks)
    state = WifiSelectionState::PASSWORD_ENTRY;
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
        "",     // No initial text
        64,     // Max password length
        false,  // Show password by default (hard keyboard to use)
        [this](const std::string& text) {
          enteredPassword = text;
          exitActivity();
        },
        [this] {
          // Password entry cancelled - go back to network list
          state = WifiSelectionState::NETWORK_LIST;
          exitActivity();
          requestUpdate();
        }));
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
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password, preserving the hidden flag
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword, isManualSsid);
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
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
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
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    // The list has networks.size() + 1 items: all visible networks plus a
    // virtual "Add hidden network" entry at the bottom.
    const size_t totalItems = networks.size() + 1;
    const bool isAddHiddenSelected = (selectedNetworkIndex >= networks.size());

    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network, add hidden network, or rescan
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (isAddHiddenSelected) {
        // "Add hidden network" selected - ask for SSID
        selectedSSID.clear();
        enteredPassword.clear();
        selectedRequiresPassword = true;
        usedSavedPassword = false;
        autoConnecting = false;
        isManualSsid = true;
        state = WifiSelectionState::SSID_ENTRY;
        enterNewActivity(new KeyboardEntryActivity(
            renderer, mappedInput, tr(STR_ENTER_WIFI_NAME),
            "",     // No initial text
            32,     // Max SSID length
            false,  // Not a password field
            [this](const std::string& text) {
              selectedSSID = text;
              exitActivity();
            },
            [this] {
              // Cancelled - clear SSID so SSID_ENTRY handler goes back to list
              selectedSSID.clear();
              isManualSsid = false;
              exitActivity();
            }));
      } else if (!networks.empty()) {
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
      // Only allow forget for real (non-virtual) network entries
      const bool hasSavedPassword =
          !networks.empty() && !isAddHiddenSelected && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Handle navigation (include the virtual "Add hidden network" entry)
    buttonNavigator.onNext([this, totalItems] {
      selectedNetworkIndex = ButtonNavigator::nextIndex(selectedNetworkIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, totalItems] {
      selectedNetworkIndex = ButtonNavigator::previousIndex(selectedNetworkIndex, totalItems);
      requestUpdate();
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(Activity::RenderLock&&) {
  // Don't render during keyboard subactivity transitions - we're just
  // transitioning from the keyboard back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY || state == WifiSelectionState::SSID_ENTRY) {
    requestUpdateAndWait();
    return;
  }

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  char countStr[32];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), networks.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WIFI_NETWORKS),
                 countStr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    cachedMacAddress.c_str());

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
    case WifiSelectionState::SSID_ENTRY:
    case WifiSelectionState::PASSWORD_ENTRY:
      // These states are handled by the early return above; never reached here
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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Always show networks + one virtual "Add hidden network" entry at the bottom
  const int totalItems = static_cast<int>(networks.size()) + 1;
  const bool isAddHiddenSelected = (selectedNetworkIndex >= networks.size());

  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedNetworkIndex,
      [this](int index) -> std::string {
        if (index < static_cast<int>(networks.size())) {
          return networks[index].ssid;
        }
        return tr(STR_ADD_HIDDEN_NETWORK);
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        if (index >= static_cast<int>(networks.size())) {
          return "";  // No detail for virtual entry
        }
        const auto& network = networks[index];
        if (network.isHidden) {
          std::string detail = std::string("+ ~ ") + (network.isEncrypted ? "* " : "");
          if (network.rssi > -100) {
            detail += getSignalStrengthIndicator(network.rssi);
          }
          return detail;
        }
        return std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
               getSignalStrengthIndicator(network.rssi);
      });

  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                   tr(STR_NETWORK_LEGEND));

  // Only show "Forget" hint for real network entries with a saved password
  const bool hasSavedPassword =
      !networks.empty() && !isAddHiddenSelected && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == WifiSelectionState::SCANNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SCANNING));
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_CONNECTING), true, EpdFontFamily::BOLD);

    std::string ssidInfo = std::string(tr(STR_TO_PREFIX)) + selectedSSID;
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

  renderer.drawCenteredText(UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderSavePrompt() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top, ssidInfo.c_str());

  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, tr(STR_SAVE_PASSWORD));

  // Draw Yes/No buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  // Draw "Yes" button
  if (savePromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_YES)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_YES));
  }

  // Draw "No" button
  if (savePromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_NO)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_NO));
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 2) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderForgetPrompt() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, tr(STR_FORGET_NETWORK), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top, ssidInfo.c_str());

  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, tr(STR_FORGET_AND_REMOVE));

  // Draw Cancel/Forget network buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  // Draw "Cancel" button
  if (forgetPromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_CANCEL)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_CANCEL));
  }

  // Draw "Forget network" button
  if (forgetPromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_FORGET_BUTTON)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_FORGET_BUTTON));
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
