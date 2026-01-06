#include "FileTransferActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <qrcode.h>

#include <cstddef>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiSelectionActivity.h"
#include "fontIds.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data) {
  // Implementation of QR code calculation
  // The structure to manage the QR code
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  Serial.printf("[%lu] [FTACT] QR Code (%lu): %s\n", millis(), data.length(), data.c_str());

  qrcode_initText(&qrcode, qrcodeBytes, 4, ECC_LOW, data.c_str());
  const uint8_t px = 6;  // pixels per module
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        renderer.fillRect(x + px * cx, y + px * cy, px, px, true);
      }
    }
  }
}
}  // namespace

void FileTransferActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileTransferActivity*>(param);
  self->displayTaskLoop();
}

void FileTransferActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  Serial.printf("[%lu] [FTACT] [MEM] Free heap at onEnter: %d bytes\n", millis(), ESP.getFreeHeap());

  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  state = FileTransferActivityState::PROTOCOL_SELECTION;
  selectedProtocol = FileTransferProtocol::HTTP;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  updateRequired = true;

  xTaskCreate(&FileTransferActivity::taskTrampoline, "FileTransferTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Launch protocol selection subactivity
  Serial.printf("[%lu] [FTACT] Launching ProtocolSelectionActivity...\n", millis());
  enterNewActivity(new ProtocolSelectionActivity(
      renderer, mappedInput, [this](const FileTransferProtocol protocol) { onProtocolSelected(protocol); }));
}

void FileTransferActivity::onExit() {
  ActivityWithSubactivity::onExit();

  Serial.printf("[%lu] [FTACT] [MEM] Free heap at onExit start: %d bytes\n", millis(), ESP.getFreeHeap());

  state = FileTransferActivityState::SHUTTING_DOWN;

  // Stop the server first (before disconnecting WiFi)
  stopServer();

  // Stop mDNS
  MDNS.end();

  // Stop DNS server if running (AP mode)
  if (dnsServer) {
    Serial.printf("[%lu] [FTACT] Stopping DNS server...\n", millis());
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }

  // CRITICAL: Wait for LWIP stack to flush any pending packets
  Serial.printf("[%lu] [FTACT] Waiting 500ms for network stack to flush pending packets...\n", millis());
  delay(500);

  // Disconnect WiFi gracefully
  if (isApMode) {
    Serial.printf("[%lu] [FTACT] Stopping WiFi AP...\n", millis());
    WiFi.softAPdisconnect(true);
  } else {
    Serial.printf("[%lu] [FTACT] Disconnecting WiFi (graceful)...\n", millis());
    WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  }
  delay(100);  // Allow disconnect frame to be sent

  Serial.printf("[%lu] [FTACT] Setting WiFi mode OFF...\n", millis());
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down

  Serial.printf("[%lu] [FTACT] [MEM] Free heap after WiFi disconnect: %d bytes\n", millis(), ESP.getFreeHeap());

  // Acquire mutex before deleting task
  Serial.printf("[%lu] [FTACT] Acquiring rendering mutex before task deletion...\n", millis());
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task
  Serial.printf("[%lu] [FTACT] Deleting display task...\n", millis());
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [FTACT] Display task deleted\n", millis());
  }

  // Delete the mutex
  Serial.printf("[%lu] [FTACT] Deleting mutex...\n", millis());
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  Serial.printf("[%lu] [FTACT] Mutex deleted\n", millis());

  Serial.printf("[%lu] [FTACT] [MEM] Free heap at onExit end: %d bytes\n", millis(), ESP.getFreeHeap());
}

void FileTransferActivity::onProtocolSelected(const FileTransferProtocol protocol) {
  Serial.printf("[%lu] [FTACT] Protocol selected: %s\n", millis(),
                protocol == FileTransferProtocol::HTTP ? "HTTP" : "FTP");

  selectedProtocol = protocol;

  // Exit protocol selection subactivity
  exitActivity();

  // Launch network mode selection
  state = FileTransferActivityState::MODE_SELECTION;
  Serial.printf("[%lu] [FTACT] Launching NetworkModeSelectionActivity...\n", millis());
  enterNewActivity(new NetworkModeSelectionActivity(
      renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
      [this]() { onGoBack(); }  // Cancel goes back to home
      ));
}

void FileTransferActivity::onNetworkModeSelected(const NetworkMode mode) {
  Serial.printf("[%lu] [FTACT] Network mode selected: %s\n", millis(),
                mode == NetworkMode::JOIN_NETWORK ? "Join Network" : "Create Hotspot");

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  // Exit mode selection subactivity
  exitActivity();

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    Serial.printf("[%lu] [FTACT] Turning on WiFi (STA mode)...\n", millis());
    WiFi.mode(WIFI_STA);

    state = FileTransferActivityState::WIFI_SELECTION;
    Serial.printf("[%lu] [FTACT] Launching WifiSelectionActivity...\n", millis());
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  } else {
    // AP mode - start access point
    state = FileTransferActivityState::AP_STARTING;
    updateRequired = true;
    startAccessPoint();
  }
}

void FileTransferActivity::onWifiSelectionComplete(const bool connected) {
  Serial.printf("[%lu] [FTACT] WifiSelectionActivity completed, connected=%d\n", millis(), connected);

  if (connected) {
    // Get connection info before exiting subactivity
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();
    isApMode = false;

    exitActivity();

    // Start mDNS for hostname resolution
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [FTACT] mDNS started: %s.local\n", millis(), AP_HOSTNAME);
    }

    // Start the server
    startServer();
  } else {
    // User cancelled - go back to mode selection
    exitActivity();
    state = FileTransferActivityState::MODE_SELECTION;
    enterNewActivity(new NetworkModeSelectionActivity(
        renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
        [this]() { onGoBack(); }));
  }
}

void FileTransferActivity::startAccessPoint() {
  Serial.printf("[%lu] [FTACT] Starting Access Point mode...\n", millis());
  Serial.printf("[%lu] [FTACT] [MEM] Free heap before AP start: %d bytes\n", millis(), ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    Serial.printf("[%lu] [FTACT] ERROR: Failed to start Access Point!\n", millis());
    onGoBack();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  Serial.printf("[%lu] [FTACT] Access Point started!\n", millis());
  Serial.printf("[%lu] [FTACT] SSID: %s\n", millis(), AP_SSID);
  Serial.printf("[%lu] [FTACT] IP: %s\n", millis(), connectedIP.c_str());

  // Start mDNS for hostname resolution
  if (MDNS.begin(AP_HOSTNAME)) {
    Serial.printf("[%lu] [FTACT] mDNS started: %s.local\n", millis(), AP_HOSTNAME);
  } else {
    Serial.printf("[%lu] [FTACT] WARNING: mDNS failed to start\n", millis());
  }

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  Serial.printf("[%lu] [FTACT] DNS server started for captive portal\n", millis());

  Serial.printf("[%lu] [FTACT] [MEM] Free heap after AP start: %d bytes\n", millis(), ESP.getFreeHeap());

  // Start the server
  startServer();
}

void FileTransferActivity::startServer() {
  Serial.printf("[%lu] [FTACT] Starting %s server...\n", millis(),
                selectedProtocol == FileTransferProtocol::HTTP ? "HTTP" : "FTP");

  if (selectedProtocol == FileTransferProtocol::HTTP) {
    // Create and start HTTP server
    webServer.reset(new CrossPointWebServer());
    webServer->begin();

    if (webServer->isRunning()) {
      state = FileTransferActivityState::SERVER_RUNNING;
      Serial.printf("[%lu] [FTACT] HTTP server started successfully\n", millis());
    } else {
      Serial.printf("[%lu] [FTACT] ERROR: Failed to start HTTP server!\n", millis());
      webServer.reset();
      onGoBack();
      return;
    }
  } else {
    // Create and start FTP server
    ftpServer.reset(new CrossPointFtpServer());
    if (ftpServer->begin()) {
      state = FileTransferActivityState::SERVER_RUNNING;
      Serial.printf("[%lu] [FTACT] FTP server started successfully\n", millis());
    } else {
      Serial.printf("[%lu] [FTACT] ERROR: Failed to start FTP server!\n", millis());
      ftpServer.reset();
      onGoBack();
      return;
    }
  }

  // Force an immediate render since we're transitioning from a subactivity
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  render();
  xSemaphoreGive(renderingMutex);
  Serial.printf("[%lu] [FTACT] Rendered File Transfer screen\n", millis());
}

void FileTransferActivity::stopServer() {
  if (webServer && webServer->isRunning()) {
    Serial.printf("[%lu] [FTACT] Stopping HTTP server...\n", millis());
    webServer->stop();
    Serial.printf("[%lu] [FTACT] HTTP server stopped\n", millis());
  }
  webServer.reset();

  if (ftpServer && ftpServer->running()) {
    Serial.printf("[%lu] [FTACT] Stopping FTP server...\n", millis());
    ftpServer->stop();
    Serial.printf("[%lu] [FTACT] FTP server stopped\n", millis());
  }
  ftpServer.reset();
}

void FileTransferActivity::loop() {
  if (subActivity) {
    // Forward loop to subactivity
    subActivity->loop();
    return;
  }

  // Handle different states
  if (state == FileTransferActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // Handle server requests
    if (selectedProtocol == FileTransferProtocol::HTTP && webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        Serial.printf("[%lu] [FTACT] WARNING: %lu ms gap since last handleClient\n", millis(),
                      timeSinceLastHandleClient);
      }

      // Call handleClient multiple times to process pending requests faster
      constexpr int HANDLE_CLIENT_ITERATIONS = 10;
      for (int i = 0; i < HANDLE_CLIENT_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
      }
      lastHandleClientTime = millis();
    } else if (selectedProtocol == FileTransferProtocol::FTP && ftpServer && ftpServer->running()) {
      ftpServer->handleClient();
    }

    // Handle exit on Back button
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }
  }
}

void FileTransferActivity::displayTaskLoop() {
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

void FileTransferActivity::render() const {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == FileTransferActivityState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer();
  } else if (state == FileTransferActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Starting Hotspot...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
  }
}

void FileTransferActivity::renderServerRunning() const {
  // Use consistent line spacing
  constexpr int LINE_SPACING = 28;  // Space between lines

  const char* protocolName = selectedProtocol == FileTransferProtocol::HTTP ? "HTTP" : "FTP";
  const std::string title = std::string("File Transfer (") + protocolName + ")";
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title.c_str(), true, EpdFontFamily::BOLD);

  if (isApMode) {
    // AP mode display - center the content block
    int startY = 55;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, "Hotspot Mode", true, EpdFontFamily::BOLD);

    std::string ssidInfo = "Network: " + connectedSSID;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, "Connect your device to this WiFi network");

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3,
                              "or scan QR code with your phone to connect to WiFi:");
    // Show QR code for WiFi
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 4, wifiConfig);

    startY += 6 * 29 + 3 * LINE_SPACING;

    // Show URL based on protocol
    std::string serverUrl;
    if (selectedProtocol == FileTransferProtocol::HTTP) {
      serverUrl = std::string("http://") + AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, serverUrl.c_str(), true, EpdFontFamily::BOLD);

      std::string ipUrl = "or http://" + connectedIP + "/";
      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, ipUrl.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "Open this URL in your browser");
    } else {
      // FTP URL with credentials
      serverUrl = std::string("ftp://") + SETTINGS.ftpUsername + ":" + SETTINGS.ftpPassword + "@" + connectedIP + "/";
      renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, serverUrl.c_str(), true, EpdFontFamily::BOLD);

      std::string ftpInfo = "User: " + SETTINGS.ftpUsername + " | Pass: " + SETTINGS.ftpPassword;
      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, ftpInfo.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "Use FTP client or scan QR code:");
    }

    // Show QR code for server URL
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 6, serverUrl);
  } else {
    // STA mode display
    const int startY = 65;

    std::string ssidInfo = "Network: " + connectedSSID;
    if (ssidInfo.length() > 28) {
      ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, startY, ssidInfo.c_str());

    std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());

    // Show server URL based on protocol
    std::string serverUrl;
    if (selectedProtocol == FileTransferProtocol::HTTP) {
      serverUrl = "http://" + connectedIP + "/";
      renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, serverUrl.c_str(), true, EpdFontFamily::BOLD);

      std::string hostnameUrl = std::string("or http://") + AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Open this URL in your browser");
    } else {
      // FTP URL with credentials
      serverUrl = std::string("ftp://") + SETTINGS.ftpUsername + ":" + SETTINGS.ftpPassword + "@" + connectedIP + "/";
      renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, serverUrl.c_str(), true, EpdFontFamily::BOLD);

      std::string ftpInfo = "User: " + SETTINGS.ftpUsername + " | Pass: " + SETTINGS.ftpPassword;
      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, ftpInfo.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Use FTP client or scan QR code:");
    }

    // Show QR code for server URL
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "or scan QR code with your phone:");
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 6, serverUrl);
  }

  const auto labels = mappedInput.mapLabels("« Exit", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
