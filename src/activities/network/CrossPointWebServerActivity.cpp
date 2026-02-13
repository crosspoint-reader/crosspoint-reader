#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <qrcode.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
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

void CrossPointWebServerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CrossPointWebServerActivity*>(param);
  self->displayTaskLoop();
}

void CrossPointWebServerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  LOG_DBG("WEBACT] [MEM", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  updateRequired = true;

  xTaskCreate(&CrossPointWebServerActivity::taskTrampoline, "WebServerActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  enterNewActivity(new NetworkModeSelectionActivity(
      renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
      [this]() { onGoBack(); }  // Cancel goes back to home
      ));
}

void CrossPointWebServerActivity::onExit() {
  ActivityWithSubactivity::onExit();

  LOG_DBG("WEBACT] [MEM", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;

  // Stop the web server first (before disconnecting WiFi)
  stopWebServer();

  // Stop mDNS
  MDNS.end();

  // Stop DNS server if running (AP mode)
  if (dnsServer) {
    LOG_DBG("WEBACT", "Stopping DNS server...");
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }

  // Brief wait for LWIP stack to flush pending packets
  delay(50);

  // Disconnect WiFi gracefully
  if (isApMode) {
    LOG_DBG("WEBACT", "Stopping WiFi AP...");
    WiFi.softAPdisconnect(true);
  } else {
    LOG_DBG("WEBACT", "Disconnecting WiFi (graceful)...");
    WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  }
  delay(30);  // Allow disconnect frame to be sent

  LOG_DBG("WEBACT", "Setting WiFi mode OFF...");
  WiFi.mode(WIFI_OFF);
  delay(30);  // Allow WiFi hardware to power down

  LOG_DBG("WEBACT] [MEM", "Free heap after WiFi disconnect: %d bytes", ESP.getFreeHeap());

  // Acquire mutex before deleting task
  LOG_DBG("WEBACT", "Acquiring rendering mutex before task deletion...");
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task
  LOG_DBG("WEBACT", "Deleting display task...");
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    LOG_DBG("WEBACT", "Display task deleted");
  }

  // Delete the mutex
  LOG_DBG("WEBACT", "Deleting mutex...");
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  LOG_DBG("WEBACT", "Mutex deleted");

  LOG_DBG("WEBACT] [MEM", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  // Exit mode selection subactivity
  exitActivity();

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    exitActivity();
    enterNewActivity(new CalibreConnectActivity(renderer, mappedInput, [this] {
      exitActivity();
      state = WebServerActivityState::MODE_SELECTION;
      enterNewActivity(new NetworkModeSelectionActivity(
          renderer, mappedInput, [this](const NetworkMode nextMode) { onNetworkModeSelected(nextMode); },
          [this]() { onGoBack(); }));
    }));
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    updateRequired = true;
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();
    isApMode = false;

    exitActivity();

    // Start mDNS for hostname resolution
    if (MDNS.begin(AP_HOSTNAME)) {
      LOG_DBG("WEBACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
    }

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    exitActivity();
    state = WebServerActivityState::MODE_SELECTION;
    enterNewActivity(new NetworkModeSelectionActivity(
        renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
        [this]() { onGoBack(); }));
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT] [MEM", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

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
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
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

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  if (MDNS.begin(AP_HOSTNAME)) {
    LOG_DBG("WEBACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
  } else {
    LOG_DBG("WEBACT", "WARNING: mDNS failed to start");
  }

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  LOG_DBG("WEBACT", "DNS server started for captive portal");

  LOG_DBG("WEBACT] [MEM", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
    LOG_DBG("WEBACT", "Rendered File Transfer screen");
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    onGoBack();
  }
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    LOG_DBG("WEBACT", "Stopping web server...");
    webServer->stop();
    LOG_DBG("WEBACT", "Web server stopped");
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  if (subActivity) {
    // Forward loop to subactivity
    subActivity->loop();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        if (wifiStatus != WL_CONNECTED) {
          LOG_DBG("WEBACT", "WiFi disconnected! Status: %d", wifiStatus);
          // Show error and exit gracefully
          state = WebServerActivityState::SHUTTING_DOWN;
          updateRequired = true;
          return;
        }
        // Log weak signal warnings
        const int rssi = WiFi.RSSI();
        if (rssi < -75) {
          LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
        }
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      esp_task_wdt_reset();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 500;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          esp_task_wdt_reset();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Force trigger an update of which buttons are being pressed so be have accurate state
          // for back button checking
          mappedInput.update();
          // Check for exit button inside loop for responsiveness
          if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            onGoBack();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // Handle exit on Back button (also check outside loop)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }
  }
}

void CrossPointWebServerActivity::displayTaskLoop() {
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

void CrossPointWebServerActivity::render() const {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer();
  } else if (state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Starting Hotspot...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
  }
}

void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data) {
  // Implementation of QR code calculation
  // The structure to manage the QR code
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  LOG_DBG("WEBACT", "QR Code (%lu): %s", data.length(), data.c_str());

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

void CrossPointWebServerActivity::renderServerRunning() const {
  // Use consistent line spacing
  constexpr int LINE_SPACING = 28;  // Space between lines

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "File Transfer", true, EpdFontFamily::BOLD);

  if (isApMode) {
    // AP mode display - center the content block
    int startY = 55;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, "Hotspot Mode", true, EpdFontFamily::BOLD);

    std::string ssidInfo = "Network: " + connectedSSID;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, "Connect your device to this WiFi network");

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3,
                              "or scan QR code with your phone to connect to Wifi.");
    // Show QR code for URL
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 4, wifiConfig);

    startY += 6 * 29 + 3 * LINE_SPACING;
    // Show primary URL (hostname)
    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str(), true, EpdFontFamily::BOLD);

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
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true, EpdFontFamily::BOLD);

    // Also show hostname URL
    std::string hostnameUrl = std::string("or http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Open this URL in your browser");

    // Show QR code for URL
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 6, webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "or scan QR code with your phone:");
  }

  const auto labels = mappedInput.mapLabels("« Exit", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
