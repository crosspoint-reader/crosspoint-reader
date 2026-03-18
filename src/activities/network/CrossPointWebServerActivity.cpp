#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <qrcode.h>

#include <cstddef>

#include "CalibreConnectActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "ScreenComponents.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BackgroundWifiService.h"
#include "util/NetworkNames.h"
#include "util/TimeSync.h"

namespace {
// AP Mode configuration
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

// Task shutdown timeout
constexpr int TASK_EXIT_TIMEOUT_MS = 500;
constexpr int TASK_EXIT_POLL_MS = 10;

bool hasStaWifiConnection() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Stop any background WiFi service — it would conflict on port 80
  if (BG_WIFI.isRunning()) {
    LOG_DBG("WEBACT", "Stopping background WiFi service for foreground use");
    BG_WIFI.stop(true);
  }

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             activityManager.goHome();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

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

  if (!isApMode && SETTINGS.keepsBackgroundServerOnWifiWhileAwake() && hasStaWifiConnection()) {
    LOG_DBG("WEBACT", "Preserving WiFi connection for background file server");
    LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
    return;
  }

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

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
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

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    startActivityForResult(
        std::make_unique<CalibreConnectActivity>(renderer, mappedInput), [this](const ActivityResult& result) {
          state = WebServerActivityState::MODE_SELECTION;

          startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                                 [this](const ActivityResult& result) {
                                    if (result.isCancelled) {
                                      activityManager.goHome();
                                    } else {
                                      onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                                    }
                                  });
        });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
    WiFi.mode(WIFI_STA);

    if (hasStaWifiConnection()) {
      connectedIP = WiFi.localIP().toString().c_str();
      connectedSSID = WiFi.SSID().c_str();
      onWifiSelectionComplete(true);
      return;
    }

    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Sync time via NTP in a background task — syncTimeWithNtpLowMemory() blocks up to 3s
    // and accesses the SD card (SETTINGS.saveToFile), so run it off the main task.
    xTaskCreate(
        [](void*) {
          TimeSync::syncTimeWithNtpLowMemory();
          vTaskDelete(nullptr);
        },
        "TimeSyncTask", 4096, nullptr, 1, nullptr);

    // Start mDNS for hostname resolution
    {
      char hostname[40];
      NetworkNames::getDeviceHostname(hostname, sizeof(hostname));
      if (MDNS.begin(hostname)) {
        LOG_DBG("WEBACT", "mDNS started: http://%s.local/", hostname);
      }
    }

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               activityManager.goHome();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  char apSsid[40];
  NetworkNames::getApSsid(apSsid, sizeof(apSsid));
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(apSsid, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(apSsid, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    activityManager.goHome();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = apSsid;

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", apSsid);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  {
    char hostname[40];
    NetworkNames::getDeviceHostname(hostname, sizeof(hostname));
    if (MDNS.begin(hostname)) {
      LOG_DBG("WEBACT", "mDNS started: http://%s.local/", hostname);
    } else {
      LOG_DBG("WEBACT", "WARNING: mDNS failed to start");
    }
  }

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  LOG_DBG("WEBACT", "DNS server started for captive portal");

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

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
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    activityManager.goHome();
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

void CrossPointWebServerActivity::updateUploadProgress() {
  if (!webServer) return;

  const auto status = webServer->getWsUploadStatus();
  bool changed = false;

  if (status.inProgress) {
    if (status.received != lastProgressReceived || status.total != lastProgressTotal ||
        status.filename != currentUploadName) {
      lastProgressReceived = status.received;
      lastProgressTotal = status.total;
      currentUploadName = status.filename;
      changed = true;
    }
  } else if (lastProgressReceived != 0 || lastProgressTotal != 0) {
    lastProgressReceived = 0;
    lastProgressTotal = 0;
    currentUploadName.clear();
    changed = true;
  }

  if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastCompleteAt) {
    lastCompleteAt = status.lastCompleteAt;
    lastCompleteName = status.lastCompleteName;
    changed = true;
  }

  // Clear completion message after 6 seconds
  if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
    lastCompleteAt = 0;
    lastCompleteName.clear();
    changed = true;
  }

  if (changed) {
    requestUpdate();
  }
}

void CrossPointWebServerActivity::loop() {
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
          requestUpdate();
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
      // Use fewer iterations for Calibre mode to allow more frequent UI updates
      const int maxIterations = (networkMode == NetworkMode::CONNECT_CALIBRE) ? 80 : 500;
      for (int i = 0; i < maxIterations && webServer->isRunning(); i++) {
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
            activityManager.goHome();
            return;
          }
        }
      }
      lastHandleClientTime = millis();

      // Update upload progress (for Calibre mode UI)
      updateUploadProgress();
    }

    // Handle exit on Back button (also check outside loop)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      activityManager.goHome();
      return;
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer();
  } else if (state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_STARTING_HOTSPOT), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
  }
}

void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data) {
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  LOG_DBG("WEBACT", "QR Code (%lu): %s", data.length(), data.c_str());

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

void CrossPointWebServerActivity::renderServerRunning() const {
  if (networkMode == NetworkMode::CONNECT_CALIBRE) {
    renderCalibreUI();
  } else {
    renderFileTransferUI();
  }
}

void CrossPointWebServerActivity::renderCalibreUI() const {
  constexpr int LINE_SPACING = 24;
  constexpr int SMALL_SPACING = 20;
  constexpr int SECTION_SPACING = 40;
  constexpr int TOP_PADDING = 14;

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Connect to Calibre", true, EpdFontFamily::BOLD);

  int y = 55 + TOP_PADDING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Network", true, EpdFontFamily::BOLD);
  y += LINE_SPACING;

  std::string ssidInfo = "Network: " + connectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, ssidInfo.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, y + LINE_SPACING, ("IP: " + connectedIP).c_str());

  y += LINE_SPACING * 2 + SECTION_SPACING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Setup", true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "1) Install CrossPoint Reader plugin");
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING, "2) Be on the same WiFi network");
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING * 2, "3) In Calibre: \"Send to device\"");
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING * 3, "Keep this screen open while sending");

  y += SMALL_SPACING * 3 + SECTION_SPACING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Status", true, EpdFontFamily::BOLD);
  y += LINE_SPACING;

  if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
    std::string label = "Receiving";
    if (!currentUploadName.empty()) {
      label += ": " + currentUploadName;
      if (label.length() > 34) {
        label.replace(31, label.length() - 31, "...");
      }
    }
    renderer.drawCenteredText(SMALL_FONT_ID, y, label.c_str());
    constexpr int barWidth = 300;
    constexpr int barHeight = 16;
    constexpr int barX = (480 - barWidth) / 2;
    ScreenComponents::drawProgressBar(renderer, barX, y + 22, barWidth, barHeight, lastProgressReceived,
                                      lastProgressTotal);
    y += 40;
  }

  if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
    std::string msg = "Received: " + lastCompleteName;
    if (msg.length() > 36) {
      msg.replace(33, msg.length() - 33, "...");
    }
    renderer.drawCenteredText(SMALL_FONT_ID, y, msg.c_str());
  }

  const auto labels = mappedInput.mapLabels("« Exit", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderFileTransferUI() const {
  constexpr int LINE_SPACING = 28;

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_FILE_TRANSFER), true, EpdFontFamily::BOLD);

  if (isApMode) {
    // AP mode display - center the content block
    int startY = 55;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_HOTSPOT_MODE), true, EpdFontFamily::BOLD);

    std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + connectedSSID;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, tr(STR_CONNECT_WIFI_HINT));

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, tr(STR_SCAN_QR_WIFI_HINT));
    // Show QR code for URL
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 4, wifiConfig);

    startY += 6 * 29 + 3 * LINE_SPACING;
    // Show primary URL (hostname)
    char _hn[40];
    NetworkNames::getDeviceHostname(_hn, sizeof(_hn));
    std::string hostnameUrl = std::string("http://") + _hn + ".local/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str(), true, EpdFontFamily::BOLD);

    // Show IP address as fallback
    std::string ipUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + connectedIP + "/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, ipUrl.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, tr(STR_OPEN_URL_HINT));

    // Show QR code for URL
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, tr(STR_SCAN_QR_HINT));
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 7, hostnameUrl);
  } else {
    // STA mode display
    const int startY = 65;

    std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + connectedSSID;
    if (ssidInfo.length() > 28) {
      ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, startY, ssidInfo.c_str());

    std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());

    // Show web server URL prominently
    std::string webInfo = "http://" + connectedIP + "/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true, EpdFontFamily::BOLD);

    // Also show hostname URL
    char _hn2[40];
    NetworkNames::getDeviceHostname(_hn2, sizeof(_hn2));
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + _hn2 + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, tr(STR_OPEN_URL_HINT));

    // Show QR code for URL
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 6, webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, tr(STR_SCAN_QR_HINT));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
