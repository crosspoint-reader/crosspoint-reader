#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstddef>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/RssFeedSync.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  uploadedFiles.clear();
  // Note: don't clear received files here — feed sync may have already run before user opened this screen
  requestUpdate();

  if (preConnected) {
    // DZ auto-connect already joined WiFi — skip mode/WiFi selection and go straight to server.
    LOG_DBG("WEBACT", "Pre-connected mode: skipping network selection");
    isApMode = false;
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    if (MDNS.begin(AP_HOSTNAME)) {
      LOG_DBG("WEBACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
    }
    startWebServer();
    RssFeedSync::startSync();
    return;
  }

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onGoHome();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  UITheme::setNetworkStatus(false, false);
  UITheme::setHttpServerActive(false);
  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;

  // Stop mDNS FIRST — must complete async teardown before stopping web server.
  // If mDNS teardown runs after stopWebServer(), it tries to send packets
  // through a dead lwIP socket context, causing a crash in mdns_free().
  LOG_DBG("WEBACT", "Stopping mDNS...");
  MDNS.end();
  delay(100);  // Allow mDNS async teardown to complete in lwIP stack

  // Stop the web server (after mDNS has fully torn down)
  stopWebServer();

  // Stop DNS server if running (AP mode)
  if (dnsServer) {
    LOG_DBG("WEBACT", "Stopping DNS server...");
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }

  // Brief wait for LWIP stack to flush remaining packets
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
                                     onGoHome();
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

    // Start mDNS for hostname resolution
    if (MDNS.begin(AP_HOSTNAME)) {
      LOG_DBG("WEBACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
    }

    // Start the web server
    startWebServer();

    // Start background RSS feed sync on non-hotspot connections
    RssFeedSync::startSync();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               onGoHome();
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
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    onGoHome();
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
    UITheme::setNetworkStatus(true, false);
    UITheme::setHttpServerActive(true);
    LOG_DBG("WEBACT", "Web server started successfully");

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    onGoHome();
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
  // Show on-screen progress while a GitHub (claw) firmware download is in progress.
  // Renders every ~2s so the display reflects current download state.
  {
    static unsigned long lastClawRender = 0;
    static CrossPointWebServer::ClawUpdateState lastRenderedState = CrossPointWebServer::ClawUpdateState::IDLE;
    const auto prog = CrossPointWebServer::getClawUpdateProgress();
    const bool active = (prog.state == CrossPointWebServer::ClawUpdateState::DOWNLOADING ||
                         prog.state == CrossPointWebServer::ClawUpdateState::CHECKING);
    if (active || prog.state != lastRenderedState) {
      if (millis() - lastClawRender > 2000 || prog.state != lastRenderedState) {
        lastClawRender = millis();
        lastRenderedState = prog.state;
        const int pageH = renderer.getScreenHeight();
        const int pageW = renderer.getScreenWidth();
        if (prog.state == CrossPointWebServer::ClawUpdateState::DOWNLOADING) {
          renderer.clearScreen();
          renderer.drawCenteredText(PULSR_10_FONT_ID, pageH / 2 - 40, "Downloading update...", true,
                                    EpdFontFamily::BOLD);
          if (prog.total > 0) {
            const int pct = (int)(prog.downloaded * 100 / prog.total);
            const int barW = pageW - 60;
            const int barH = 14;
            const int barX = 30;
            const int barY = pageH / 2;
            renderer.drawRect(barX, barY, barW, barH, 0);
            renderer.fillRect(barX, barY, barW * pct / 100, barH, 0);
            char pctStr[32];
            snprintf(pctStr, sizeof(pctStr), "%d%%  (%.1f / %.1f MB)", pct, prog.downloaded / 1048576.0f,
                     prog.total / 1048576.0f);
            renderer.drawCenteredText(SMALL_FONT_ID, pageH / 2 + barH + 12, pctStr);
          }
          if (!prog.version.empty()) {
            char verBuf[64];
            snprintf(verBuf, sizeof(verBuf), "Version: %s", prog.version.c_str());
            renderer.drawCenteredText(SMALL_FONT_ID, pageH / 2 + 50, verBuf);
          }
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        } else if (prog.state == CrossPointWebServer::ClawUpdateState::CHECKING) {
          renderer.clearScreen();
          renderer.drawCenteredText(PULSR_10_FONT_ID, pageH / 2, "Checking for updates...", true, EpdFontFamily::BOLD);
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        }
      }
    }
  }

  // If a DZ firmware flash was requested, exit cleanly first so onExit() stops
  // the web server and disconnects WiFi before the main loop kills WiFi.
  // Without this, killing WiFi while the TCP stack is live causes a panic.
  extern volatile bool dzFlashRequested;
  if (dzFlashRequested) {
    onGoHome();
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
            onGoHome();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // Handle exit on Back button (also check outside loop)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    // Confirm button = manually trigger RSS feed sync
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      RssFeedSync::startSync();
      requestUpdate();
    }

    // Animate RSS sync indicator while feed is active (connected or syncing)
    if (RssFeedSync::isFeedActive()) {
      const unsigned long now = millis();
      static unsigned long lastSyncRender = 0;
      if (now - lastSyncRender > 600) {
        lastSyncRender = now;
        requestUpdate();
      }
    } else {
      static bool wasSyncing = false;
      if (wasSyncing) {
        wasSyncing = false;
        requestUpdate();  // final refresh to clear the indicator
      }
      wasSyncing = RssFeedSync::isSyncing();  // always false here, resets for next sync
    }

    // Redraw when feed delivers a new file (event-driven via dirty flag)
    if (UITheme::consumeReceivedFileDirty()) requestUpdate();

    // Monitor upload status and trigger display refresh on file close only
    // Rate-limited to avoid excessive e-ink refreshes
    if (webServer) {
      const auto uploadStatus = webServer->getUploadStatus();
      const unsigned long now = millis();
      const bool fileCompleted = uploadStatus.lastCompleteAt > lastKnownCompleteAt;

      if (fileCompleted) {
        if (!uploadStatus.lastCompleteName.empty()) {
          uploadedFiles.push_back(uploadStatus.lastCompleteName);
          UITheme::addReceivedFile(uploadStatus.lastCompleteName);
        }
        lastKnownCompleteAt = uploadStatus.lastCompleteAt;
        lastUploadInProgress = uploadStatus.inProgress;
        lastTransferUpdateTime = now;
        requestUpdate();
      }
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    if (state == WebServerActivityState::SERVER_RUNNING) {
      renderServerRunning();
    } else {
      const auto height = renderer.getLineHeight(PULSR_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(PULSR_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Build header title with live connection/transfer status
  const auto uploadStatus = webServer ? webServer->getUploadStatus() : CrossPointWebServer::WsUploadStatus{};

  // Sync network status so the PULSR indicator reflects the current transfer state.
  UITheme::setNetworkStatus(true, uploadStatus.inProgress);

  std::string headerTitle = isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER);
  if (uploadStatus.inProgress) {
    // Keep title static during transfer — e-ink too slow for mid-transfer updates
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle.c_str(), nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  // Center QR codes and text within the content area (to the right of any left bar)
  const int contentLeft = metrics.contentSidePadding;
  const int contentW = pageWidth - contentLeft;

  // Draw centered text within the content area, wrapping to two lines if needed.
  // Returns the total pixel height consumed (one or two lines).
  auto drawCenteredWrapped = [&](int fontId, int y, const char* text, bool black,
                                 EpdFontFamily::Style style = EpdFontFamily::REGULAR) -> int {
    const int lineH = renderer.getTextHeight(fontId);
    const int fullW = renderer.getTextWidth(fontId, text, style);
    if (fullW <= contentW) {
      renderer.drawText(fontId, contentLeft + (contentW - fullW) / 2, y, text, black, style);
      return lineH;
    }
    // Find the last word boundary where the first line still fits.
    std::string s(text);
    size_t breakAt = 0;
    size_t pos = 0;
    while ((pos = s.find(' ', pos)) != std::string::npos) {
      const int w = renderer.getTextWidth(fontId, s.substr(0, pos).c_str(), style);
      if (w <= contentW) breakAt = pos;
      pos++;
    }
    if (breakAt == 0) breakAt = s.size();  // one word too long — draw as-is
    const std::string line1 = s.substr(0, breakAt);
    const std::string line2 = (breakAt < s.size()) ? s.substr(breakAt + 1) : "";
    const int w1 = renderer.getTextWidth(fontId, line1.c_str(), style);
    renderer.drawText(fontId, contentLeft + (contentW - w1) / 2, y, line1.c_str(), black, style);
    if (!line2.empty()) {
      const int w2 = renderer.getTextWidth(fontId, line2.c_str(), style);
      renderer.drawText(fontId, contentLeft + (contentW - w2) / 2, y + lineH + 2, line2.c_str(), black, style);
      return lineH * 2 + 2;
    }
    return lineH;
  };

  if (isApMode) {
    // AP mode: two QR codes stacked vertically, text centered below each.

    // ── Section 1: WiFi connection ───────────────────────────────────────────
    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, tr(STR_CONNECT_WIFI_HINT), true, EpdFontFamily::BOLD);
    startY += metrics.verticalSpacing;

    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(contentLeft + (contentW - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing;

    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, connectedSSID.c_str(), true);
    startY += metrics.verticalSpacing * 3;

    // ── Section 2: Web URL ───────────────────────────────────────────────────
    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += metrics.verticalSpacing;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";

    // Show QR code for URL
    const Rect qrBoundsUrl(contentLeft + (contentW - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing;

    const bool apTextBlack = !UITheme::isInverted();  // white text in dark mode
    startY += drawCenteredWrapped(PULSR_12_FONT_ID, startY, hostnameUrl.c_str(), apTextBlack);
    startY += metrics.verticalSpacing;
    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, ipUrl.c_str(), apTextBlack);
    startY += metrics.verticalSpacing;

    // Completed uploads list (oldest first), left-justified in PULSR font
    const int pulsrLineH = renderer.getLineHeight(PULSR_12_FONT_ID);
    const int maxTextW = pageWidth - contentLeft - metrics.contentSidePadding;
    auto truncate = [&](const std::string& s) {
      if (renderer.getTextWidth(PULSR_12_FONT_ID, s.c_str()) <= maxTextW) return s;
      std::string t = s;
      while (!t.empty() && renderer.getTextWidth(PULSR_12_FONT_ID, (t + "…").c_str()) > maxTextW) t.pop_back();
      return t + "…";
    };
    for (const auto& name : UITheme::getReceivedFiles()) {
      renderer.drawText(PULSR_12_FONT_ID, contentLeft, startY, truncate(name).c_str(), apTextBlack);
      startY += pulsrLineH;
    }
    // In-progress upload
    if (uploadStatus.inProgress && !uploadStatus.filename.empty()) {
      const std::string inProg = "● " + uploadStatus.filename;
      renderer.drawText(PULSR_12_FONT_ID, contentLeft, startY, truncate(inProg).c_str(),
                        apTextBlack, EpdFontFamily::BOLD);
    }
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode: one QR code centered in the content area.
    // In dark theme, content area is black — use white text (black=false) for readability.
    const bool textBlack = !UITheme::isInverted();
    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), textBlack, EpdFontFamily::BOLD);
    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), textBlack, EpdFontFamily::BOLD);
    startY += metrics.verticalSpacing * 2;

    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds(contentLeft + (contentW - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    startY += drawCenteredWrapped(PULSR_12_FONT_ID, startY, webInfo.c_str(), textBlack);
    startY += metrics.verticalSpacing;
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    startY += drawCenteredWrapped(PULSR_10_FONT_ID, startY, hostnameUrl.c_str(), textBlack);
    startY += metrics.verticalSpacing;

    // Completed uploads list (oldest first), left-justified in PULSR font
    const int pulsrLineH = renderer.getLineHeight(PULSR_12_FONT_ID);
    const int maxTextW = pageWidth - contentLeft - metrics.contentSidePadding;
    auto truncate = [&](const std::string& s) {
      if (renderer.getTextWidth(PULSR_12_FONT_ID, s.c_str()) <= maxTextW) return s;
      std::string t = s;
      while (!t.empty() && renderer.getTextWidth(PULSR_12_FONT_ID, (t + "…").c_str()) > maxTextW) t.pop_back();
      return t + "…";
    };
    for (const auto& name : UITheme::getReceivedFiles()) {
      renderer.drawText(PULSR_12_FONT_ID, contentLeft, startY, truncate(name).c_str(), textBlack);
      startY += pulsrLineH;
    }
    // In-progress upload
    if (uploadStatus.inProgress && !uploadStatus.filename.empty()) {
      const std::string inProg = "● " + uploadStatus.filename;
      renderer.drawText(PULSR_12_FONT_ID, contentLeft, startY, truncate(inProg).c_str(),
                        textBlack, EpdFontFamily::BOLD);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_CHECK_FEED), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
