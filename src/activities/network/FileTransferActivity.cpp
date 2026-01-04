#include "FileTransferActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <qrcode.h>

#include <cstddef>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "WifiSelectionActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
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
}  // namespace

void FileTransferActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileTransferActivity*>(param);
  self->displayTaskLoop();
}

void FileTransferActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onEnter: %d bytes\n", millis(), ESP.getFreeHeap());

  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  state = FileTransferActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  updateRequired = true;

  xTaskCreate(&FileTransferActivity::taskTrampoline, "WebServerActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Launch network mode selection subactivity
  Serial.printf("[%lu] [WEBACT] Launching NetworkModeSelectionActivity...\n", millis());
  enterNewActivity(new NetworkModeSelectionActivity(
      renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
      [this]() { onGoBack(); }  // Cancel goes back to home
      ));
}

void FileTransferActivity::onExit() {
  ActivityWithSubactivity::onExit();

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onExit start: %d bytes\n", millis(), ESP.getFreeHeap());

  state = FileTransferActivityState::SHUTTING_DOWN;

  // Stop the file transfer servers first (before disconnecting WiFi)
  stopHttpServer();
  stopFtpServer();

  // Stop mDNS
  MDNS.end();

  // Stop DNS server if running (AP mode)
  if (dnsServer) {
    Serial.printf("[%lu] [WEBACT] Stopping DNS server...\n", millis());
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }

  // CRITICAL: Wait for LWIP stack to flush any pending packets
  Serial.printf("[%lu] [WEBACT] Waiting 500ms for network stack to flush pending packets...\n", millis());
  delay(500);

  // Disconnect WiFi gracefully
  if (isApMode) {
    Serial.printf("[%lu] [WEBACT] Stopping WiFi AP...\n", millis());
    WiFi.softAPdisconnect(true);
  } else {
    Serial.printf("[%lu] [WEBACT] Disconnecting WiFi (graceful)...\n", millis());
    WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  }
  delay(100);  // Allow disconnect frame to be sent

  Serial.printf("[%lu] [WEBACT] Setting WiFi mode OFF...\n", millis());
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after WiFi disconnect: %d bytes\n", millis(), ESP.getFreeHeap());

  // Acquire mutex before deleting task
  Serial.printf("[%lu] [WEBACT] Acquiring rendering mutex before task deletion...\n", millis());
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task
  Serial.printf("[%lu] [WEBACT] Deleting display task...\n", millis());
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [WEBACT] Display task deleted\n", millis());
  }

  // Delete the mutex
  Serial.printf("[%lu] [WEBACT] Deleting mutex...\n", millis());
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  Serial.printf("[%lu] [WEBACT] Mutex deleted\n", millis());

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onExit end: %d bytes\n", millis(), ESP.getFreeHeap());
}

void FileTransferActivity::onNetworkModeSelected(const NetworkMode mode) {
  Serial.printf("[%lu] [WEBACT] Network mode selected: %s\n", millis(),
                mode == NetworkMode::JOIN_NETWORK ? "Join Network" : "Create Hotspot");

  // Check for WiFi/BLE mutual exclusion
  if (SETTINGS.bluetoothEnabled) {
    Serial.printf("[%lu] [WEBACT] ERROR: Cannot start WiFi while Bluetooth is enabled\n", millis());
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(
        renderer, mappedInput, "Disable Bluetooth first\n\nGo to Settings > Bluetooth"));
    return;
  }

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  // Exit mode selection subactivity
  exitActivity();

  // Launch protocol selection subactivity
  state = FileTransferActivityState::PROTOCOL_SELECTION;
  Serial.printf("[%lu] [WEBACT] Launching ProtocolSelectionActivity...\n", millis());
  enterNewActivity(new ProtocolSelectionActivity(
      renderer, mappedInput, [this](const FileTransferProtocol protocol) { onProtocolSelected(protocol); },
      [this]() { onGoBack(); }));
}

void FileTransferActivity::onProtocolSelected(const FileTransferProtocol protocol) {
  Serial.printf("[%lu] [WEBACT] Protocol selected: %s\n", millis(),
                protocol == FileTransferProtocol::HTTP ? "HTTP" : "FTP");

  selectedProtocol = protocol;

  // Exit protocol selection subactivity
  exitActivity();

  if (networkMode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    Serial.printf("[%lu] [WEBACT] Turning on WiFi (STA mode)...\n", millis());
    WiFi.mode(WIFI_STA);

    state = FileTransferActivityState::WIFI_SELECTION;
    Serial.printf("[%lu] [WEBACT] Launching WifiSelectionActivity...\n", millis());
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
  Serial.printf("[%lu] [WEBACT] WifiSelectionActivity completed, connected=%d\n", millis(), connected);

  if (connected) {
    // Get connection info before exiting subactivity
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();
    isApMode = false;

    exitActivity();

    // Start mDNS for hostname resolution
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
    }

    // Start the file transfer server
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
  Serial.printf("[%lu] [WEBACT] Starting Access Point mode...\n", millis());
  Serial.printf("[%lu] [WEBACT] [MEM] Free heap before AP start: %d bytes\n", millis(), ESP.getFreeHeap());

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
    Serial.printf("[%lu] [WEBACT] ERROR: Failed to start Access Point!\n", millis());
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

  Serial.printf("[%lu] [WEBACT] Access Point started!\n", millis());
  Serial.printf("[%lu] [WEBACT] SSID: %s\n", millis(), AP_SSID);
  Serial.printf("[%lu] [WEBACT] IP: %s\n", millis(), connectedIP.c_str());

  // Start mDNS for hostname resolution
  if (MDNS.begin(AP_HOSTNAME)) {
    Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
  } else {
    Serial.printf("[%lu] [WEBACT] WARNING: mDNS failed to start\n", millis());
  }

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  Serial.printf("[%lu] [WEBACT] DNS server started for captive portal\n", millis());

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after AP start: %d bytes\n", millis(), ESP.getFreeHeap());

  // Start the file transfer server
  startServer();
}

void FileTransferActivity::startServer() {
  if (selectedProtocol == FileTransferProtocol::HTTP) {
    Serial.printf("[%lu] [WEBACT] Starting HTTP server...\n", millis());

    // Create the HTTP server instance
    httpServer.reset(new CrossPointWebServer());
    httpServer->begin();

    if (httpServer->isRunning()) {
      state = FileTransferActivityState::SERVER_RUNNING;
      serverStartTime = millis();  // Track when server started
      Serial.printf("[%lu] [WEBACT] HTTP server started successfully\n", millis());

      // Force an immediate render since we're transitioning from a subactivity
      // that had its own rendering task. We need to make sure our display is shown.
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
      Serial.printf("[%lu] [WEBACT] Rendered File Transfer screen\n", millis());
    } else {
      Serial.printf("[%lu] [WEBACT] ERROR: Failed to start HTTP server!\n", millis());
      httpServer.reset();
      onGoBack();
    }
  } else {
    Serial.printf("[%lu] [WEBACT] Starting FTP server...\n", millis());

    // Create the FTP server instance
    ftpServer.reset(new CrossPointFtpServer());
    ftpServer->begin();

    if (ftpServer->isRunning()) {
      state = FileTransferActivityState::SERVER_RUNNING;
      serverStartTime = millis();  // Track when server started
      Serial.printf("[%lu] [WEBACT] FTP server started successfully\n", millis());

      // Force an immediate render
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
      Serial.printf("[%lu] [WEBACT] Rendered File Transfer screen\n", millis());
    } else {
      Serial.printf("[%lu] [WEBACT] ERROR: Failed to start FTP server!\n", millis());
      ftpServer.reset();
      onGoBack();
    }
  }
}

void FileTransferActivity::stopHttpServer() {
  if (httpServer && httpServer->isRunning()) {
    Serial.printf("[%lu] [WEBACT] Stopping HTTP server...\n", millis());
    httpServer->stop();
    Serial.printf("[%lu] [WEBACT] HTTP server stopped\n", millis());
  }
  httpServer.reset();
}

void FileTransferActivity::stopFtpServer() {
  if (ftpServer && ftpServer->isRunning()) {
    Serial.printf("[%lu] [WEBACT] Stopping FTP server...\n", millis());
    ftpServer->stop();
    Serial.printf("[%lu] [WEBACT] FTP server stopped\n", millis());
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

    // Handle file transfer server requests - call handleClient multiple times per loop
    // to improve responsiveness and upload throughput
    const bool httpRunning = httpServer && httpServer->isRunning();
    const bool ftpRunning = ftpServer && ftpServer->isRunning();

    if (httpRunning || ftpRunning) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        Serial.printf("[%lu] [WEBACT] WARNING: %lu ms gap since last handleClient\n", millis(),
                      timeSinceLastHandleClient);
      }

      // Call handleClient multiple times to process pending requests faster
      // This is critical for upload performance - file uploads send data
      // in chunks and each handleClient() call processes incoming data
      // Reduced from 10 to 3 to prevent watchdog timer issues
      constexpr int HANDLE_CLIENT_ITERATIONS = 3;
      for (int i = 0; i < HANDLE_CLIENT_ITERATIONS; i++) {
        if (httpRunning && httpServer->isRunning()) {
          httpServer->handleClient();
        } else if (ftpRunning && ftpServer->isRunning()) {
          ftpServer->handleClient();
        }
        // Feed the watchdog timer between iterations to prevent resets
        esp_task_wdt_reset();
        // Yield to other tasks to prevent starvation
        yield();
      }
      lastHandleClientTime = millis();
    }

    // Check auto-shutdown timer if schedule is enabled
    if (SETTINGS.scheduleEnabled && serverStartTime > 0) {
      const unsigned long serverUptime = millis() - serverStartTime;
      const unsigned long shutdownTimeout = SETTINGS.getAutoShutdownMs();
      
      if (serverUptime >= shutdownTimeout) {
        Serial.printf("[%lu] [WEBACT] Auto-shutdown triggered after %lu ms\n", millis(), serverUptime);
        onGoBack();
        return;
      }
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
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Starting Hotspot...", true, BOLD);
    renderer.displayBuffer();
  }
}

void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data) {
  // Implementation of QR code calculation
  // The structure to manage the QR code
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  Serial.printf("[%lu] [WEBACT] QR Code (%lu): %s\n", millis(), data.length(), data.c_str());

  qrcode_initText(&qrcode, qrcodeBytes, 4, ECC_LOW, data.c_str());
  const uint8_t px = 6;  // pixels per module
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        // Serial.print("**");
        renderer.fillRect(x + px * cx, y + px * cy, px, px, true);
      } else {
        // Serial.print("  ");
      }
    }
    // Serial.print("\n");
  }
}

void FileTransferActivity::renderServerRunning() const {
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "File Transfer", true, BOLD);

  if (selectedProtocol == FileTransferProtocol::HTTP) {
    renderHttpServerRunning();
  } else {
    renderFtpServerRunning();
  }
}

void FileTransferActivity::renderHttpServerRunning() const {
  // Use consistent line spacing
  constexpr int LINE_SPACING = 28;  // Space between lines

  if (isApMode) {
    // AP mode display - center the content block
    int startY = 55;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, "Hotspot Mode", true, BOLD);

    std::string ssidInfo = "Network: " + connectedSSID;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, "Connect your device to this WiFi network");

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3,
                              "or scan QR code with your phone to connect to Wifi.");
    // Show QR code for URL
    std::string wifiConfig = std::string("WIFI:T:WPA;S:") + connectedSSID + ";P:" + "" + ";;";
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 4, wifiConfig);

    startY += 6 * 29 + 3 * LINE_SPACING;
    // Show primary URL (hostname)
    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str(), true, BOLD);

    // Show IP address as fallback
    std::string ipUrl = "or http://" + connectedIP + "/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, ipUrl.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "Open this URL in your browser");

    // Show QR code for URL
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, "or scan QR code with your phone:");
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 7, hostnameUrl);
  } else {
    // STA mode display (original behavior)
    const int startY = 65;

    std::string ssidInfo = "Network: " + connectedSSID;
    if (ssidInfo.length() > 28) {
      ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, startY, ssidInfo.c_str());

    std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());

    // Show web server URL prominently
    std::string webInfo = "http://" + connectedIP + "/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true, BOLD);

    // Also show hostname URL
    std::string hostnameUrl = std::string("or http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Open this URL in your browser");

    // Show QR code for URL
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 6, webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "or scan QR code with your phone:");
  }

  const auto labels = mappedInput.mapLabels("« Exit", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FileTransferActivity::renderFtpServerRunning() const {
  // Use consistent line spacing
  constexpr int LINE_SPACING = 28;  // Space between lines

  if (isApMode) {
    // AP mode display
    int startY = 55;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, "Hotspot Mode", true, BOLD);

    std::string ssidInfo = "Network: " + connectedSSID;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, "Connect your device to this WiFi network");

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3,
                              "or scan QR code with your phone to connect to WiFi.");
    // Show QR code for WiFi
    std::string wifiConfig = std::string("WIFI:T:WPA;S:") + connectedSSID + ";P:" + "" + ";;";
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 4, wifiConfig);

    startY += 6 * 29 + 3 * LINE_SPACING;

    // Show FTP server info
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, "FTP Server", true, BOLD);

    std::string ftpInfo = "ftp://" + connectedIP + "/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 4, ftpInfo.c_str(), true, BOLD);

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "Connect with FTP client:");
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, "Username: crosspoint");
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 7, "Password: reader");
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

    // Show FTP server info
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, "FTP Server", true, BOLD);

    std::string ftpInfo = "ftp://" + connectedIP + "/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, ftpInfo.c_str(), true, BOLD);

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Use FTP client to connect:");
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "Username: crosspoint");
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, "Password: reader");

    // Show QR code for FTP URL
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 8, ftpInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 7, "or scan QR code with your phone:");
  }

  const auto labels = mappedInput.mapLabels("« Exit", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
