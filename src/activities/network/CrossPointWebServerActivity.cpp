#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstddef>
#include <new>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "activities/network/SignalStrengthWidget.h"
#include "components/TouchExitButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 2;  // reduce from default 4 to save resources
// Fixed AP addressing (the esp32 softAP default, pinned explicitly) so the
// QR/URL screen can be painted before the WiFi stack starts.
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_NETMASK(255, 255, 255, 0);
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) {
    return;
  }
  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  buffersReleased = false;
  connectedIP.clear();
  connectedSSID.clear();
  requestUpdate();

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput, initialSelection),
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

  state = WebServerActivityState::SHUTTING_DOWN;
  stopDnsServer();
  session.end();
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
    // BEFORE the radio: association itself is allocation-heavy, and on a lean heap it is the
    // join that fails, not the server that follows it.
    session.prepareRadio();
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
    isApMode = false;
    startWebServer();
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

  // Everything on the server screen is known before WiFi starts: the SSID is a
  // constant and the AP IP is pinned via softAPConfig below. Paint and release
  // the frame buffers (~100KB) first so the WiFi stack + mDNS + DNS server
  // (~66KB) start against a roomy heap; painting after AP start left the X3
  // with <5KB free and aborted in the QR render.
  connectedSSID = AP_SSID;
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  connectedIP = ipStr;
  showServerScreenAndReleaseBuffers();

  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP. The screen painted above ran ServerSession::prepareRadio().
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

  // Pin the AP address to what the already-painted screen promises.
  if (!WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK)) {
    LOG_ERR("WEBACT", "ERROR: softAPConfig failed");
  }

  delay(100);  // Wait for AP to fully initialize

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  // Raw nothrow new: owned by the module-scope dnsServer pointer, freed in
  // stopDnsServer(). On OOM the captive portal degrades to direct IP/mDNS access.
  dnsServer = new (std::nothrow) DNSServer();
  if (dnsServer) {
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(DNS_PORT, "*", AP_IP);
    LOG_DBG("WEBACT", "DNS server started for captive portal");
  } else {
    LOG_ERR("WEBACT", "OOM allocating DNS server, captive portal disabled");
  }

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::showServerScreenAndReleaseBuffers() {
  // The AP path reaches this before WiFi.mode(WIFI_AP), so here it does the real work; the STA
  // path already ran it before the join (it cannot wait for this screen, which needs the assigned
  // IP: an X3 then failed to associate at all), and this only re-trims. Either way the memory is
  // gone before the radio, which is the invariant worth keeping.
  session.prepareRadio();

  // Set running state now so the paint below uses the correct UI branch.
  state = WebServerActivityState::SERVER_RUNNING;
  if (!isApMode) {
    currentRssi = WiFi.RSSI();
    lastRssiUpdateTime = millis();
  }

  // Paint the QR / URL screen while both frame buffers are still available,
  // then release them. The e-ink controller retains the image in its own RAM —
  // no framebuffer needed after displayBuffer().
  LOG_DBG("WEBACT", "Free heap before frame buffer release: %d bytes", ESP.getFreeHeap());
  renderer.clearScreen();
  renderServerRunning();
  renderer.displayBuffer();
  buffersReleased = true;
  renderer.releaseFrameBuffers();
  LOG_DBG("WEBACT", "Free heap after frame buffer release: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Both paths have already freed the memory the radio needed (ServerSession::prepareRadio). What
  // is left is the final paint plus releasing BOTH buffers, and only that needs the assigned IP,
  // which is why the STA path reaches it here rather than before the join.
  if (!buffersReleased) {
    showServerScreenAndReleaseBuffers();
  }
  if (!session.start(isApMode)) onGoHome();
}

void CrossPointWebServerActivity::loop() {
  if (state != WebServerActivityState::SERVER_RUNNING) return;

  // Handle DNS requests for captive portal (AP mode only)
  if (isApMode && dnsServer) {
    dnsServer->processNextRequest();
  }

  // STA mode: a drop used to end the session on the spot, and badly: the state went to
  // SHUTTING_DOWN, which loop() ignores -- Back included -- and render() cannot paint with the
  // buffers released, so the device sat on a dead File Transfer screen until it was switched off.
  // Now the server and Back keep working while the driver reconnects, and a link still gone after
  // the grace period ends the session as Back does: restart to Home. The screen cannot be
  // repainted to show the loss, nor a new address if the link comes back on one; the log has both.
  //
  // RSSI indicator update intentionally removed: the 640ms e-ink refresh every 5 seconds blocked
  // handleClient, fragmented heap, and gave no meaningful benefit since the user is interacting
  // with the browser, not watching the device screen.
  if (!isApMode && session.running() && session.link(connectedIP) == WifiLinkWatch::Link::Gone) {
    LOG_DBG("WEBACT", "Leaving File Transfer: the WiFi did not come back");
    onGoHome();
    return;
  }

  // Many rounds per pass, for throughput: this screen is never repainted, so there is nothing else
  // for the loop to do.
  if (!session.serve(500)) onGoHome();
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Frame buffers are released before the web server starts (in startWebServer).
  // Any render triggered after that point must not touch the null framebuffer.
  if (buffersReleased) return;

  // Only render our own UI when server is running.
  // Subactivities handle their own rendering.
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect contentRect = UITheme::getContentRect(renderer, true, false);

    GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                   isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);

    if (state == WebServerActivityState::SERVER_RUNNING) {
      GUI.drawSubHeader(
          renderer,
          Rect{contentRect.x, metrics.topPadding + metrics.headerHeight, contentRect.width, metrics.tabBarHeight},
          connectedSSID.c_str());
      renderServerRunning();
    } else {
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = contentRect.y + (contentRect.height - height) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }
    renderer.displayBuffer();
  }
}

namespace {}  // namespace

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);
  GUI.drawSubHeader(
      renderer, Rect{contentRect.x, metrics.topPadding + metrics.headerHeight, contentRect.width, metrics.tabBarHeight},
      connectedSSID.c_str());

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    // AP mode display
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for Wifi
    // follows spec at https://github.com/zxing/zxing/wiki/Barcode-Contents#wi-fi-network-config-android-ios-11
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

    // Show network name
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      connectedSSID.c_str());

    startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;

    // Show primary URL (hostname)
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    std::string hostnameUrl = std::string("http://") + ServerSession::HOSTNAME + ".local/";
    std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";

    // Show QR code for URL
    const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);

    // Show IP address as fallback
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      hostnameUrl.c_str());
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 100,
                      ipUrl.c_str());

    const int signalHeight = 22;
    const int signalWidth = contentRect.width - metrics.contentSidePadding * 2;
    const int signalY = startY + 120;
    drawWifiSignalStrength(renderer, contentRect.x + metrics.contentSidePadding, signalY, signalWidth, signalHeight, 0);
    renderer.drawCenteredText(SMALL_FONT_ID, signalY + signalHeight + 2, tr(STR_HOTSPOT_MODE));
    TouchExitButton::draw(
        renderer, mappedInput, contentRect.x + metrics.contentSidePadding,
        signalY + signalHeight + 2 + renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing * 2, signalWidth);
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode display (original behavior)
    // std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for URL
    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds(contentRect.x + (contentRect.width - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    // Show web server URL prominently
    renderer.drawCenteredText(UI_10_FONT_ID, startY, webInfo.c_str(), true);
    startY += height10 + 5;

    // Also show hostname URL
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + ServerSession::HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY, hostnameUrl.c_str(), true);

    // AP mode: no external RSSI metric available, but keep UI spacing consistent.
  }

  if (!isApMode) {
    const int signalHeight = 22;
    const int signalWidth = contentRect.width - metrics.contentSidePadding * 2;
    const int signalY = startY + height10 + metrics.verticalSpacing * 2;
    drawWifiSignalStrength(renderer, contentRect.x + metrics.contentSidePadding, signalY, signalWidth, signalHeight,
                           currentRssi);
    renderer.drawCenteredText(SMALL_FONT_ID, signalY + signalHeight + 2, rssiLabel(currentRssi).c_str(), true);
    TouchExitButton::draw(
        renderer, mappedInput, contentRect.x + metrics.contentSidePadding,
        signalY + signalHeight + 2 + renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing * 2, signalWidth);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
