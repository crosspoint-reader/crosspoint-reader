#include "SshServerActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

constexpr unsigned long STATUS_POLL_MS = 500;
constexpr unsigned long COMPLETE_BANNER_MS = 6000;

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    LOG_DBG(tag, "mDNS started: %s.local", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void SshServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("SSHACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Reset state
  state = SshServerActivityState::MODE_SELECTION;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastStatus = SshServer::TransferStatus{};
  lastStatusPollAt = 0;
  generatePassword();
  requestUpdate();

  launchModeSelection();
}

void SshServerActivity::onExit() {
  Activity::onExit();

  LOG_DBG("SSHACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = SshServerActivityState::SHUTTING_DOWN;
  sshServer.reset();  // stops the server task before WiFi goes away
  MDNS.end();

  // Skip reboot if WiFi was never activated (e.g. user backed out of mode selection).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestart();
  }

  LOG_DBG("SSHACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void SshServerActivity::launchModeSelection() {
  state = SshServerActivityState::MODE_SELECTION;
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onGoHome();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void SshServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);
  LOG_DBG("SSHACT", "Network mode selected: %s", isApMode ? "Create Hotspot" : "Join Network");

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    WiFi.mode(WIFI_STA);

    state = SshServerActivityState::WIFI_SELECTION;
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
    state = SshServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void SshServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("SSHACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    isApMode = false;
    restartMdns(AP_HOSTNAME, "SSHACT");
    startSshServer();
  } else {
    // User cancelled - go back to mode selection
    launchModeSelection();
  }
}

void SshServerActivity::startAccessPoint() {
  LOG_DBG("SSHACT", "Starting Access Point mode...");

  WiFi.mode(WIFI_AP);
  delay(100);

  // Open network (no password) for ease of use
  if (!WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    LOG_ERR("SSHACT", "ERROR: Failed to start Access Point!");
    onGoHome();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  LOG_DBG("SSHACT", "Access Point started, SSID: %s, IP: %s", AP_SSID, connectedIP.c_str());

  restartMdns(AP_HOSTNAME, "SSHACT");
  startSshServer();
}

void SshServerActivity::startSshServer() {
  LOG_DBG("SSHACT", "Starting SSH server...");

  sshServer = std::make_unique<SshServer>();
  if (sshServer->begin(sessionPassword)) {
    state = SshServerActivityState::SERVER_RUNNING;
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);
    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task.
    requestUpdate();
  } else {
    LOG_ERR("SSHACT", "ERROR: Failed to start SSH server!");
    sshServer.reset();
    onGoHome();
  }
}

void SshServerActivity::generatePassword() {
  // Session password shown on screen; avoids easily-confused characters.
  static constexpr char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  constexpr size_t PASSWORD_LEN = 8;
  for (size_t i = 0; i < PASSWORD_LEN; i++) {
    sessionPassword[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
  }
  sessionPassword[PASSWORD_LEN] = '\0';
}

void SshServerActivity::loop() {
  if (state != SshServerActivityState::SERVER_RUNNING) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const unsigned long now = millis();
  if (now - lastStatusPollAt >= STATUS_POLL_MS) {
    lastStatusPollAt = now;
    if (!isApMode) {
      monitorWifi();
    }
    pollServerStatus();
  }
}

void SshServerActivity::monitorWifi() {
  const wl_status_t wifiStatus = WiFi.status();
  // Driver auto-reconnect handles retries; abandon (via onGoHome) only
  // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
  bool repaint = false;
  if (wifiStatus != WL_CONNECTED) {
    if (consecutiveDisconnects == 0) {
      firstDisconnectAt = millis();
      repaint = true;
    }
    consecutiveDisconnects++;
    if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
      LOG_DBG("SSHACT", "WiFi unavailable for >%lu s; leaving", WIFI_ABANDON_MS / 1000UL);
      state = SshServerActivityState::SHUTTING_DOWN;
      onGoHome();
      return;
    }
  } else {
    if (consecutiveDisconnects > 0) {
      LOG_DBG("SSHACT", "WiFi recovered after %lu ms", millis() - firstDisconnectAt);
      repaint = true;
    }
    consecutiveDisconnects = 0;
    firstDisconnectAt = 0;
    const int bars = barsForRssi(WiFi.RSSI(), lastWifiBars);
    if (bars != lastWifiBars) {
      lastWifiBars = bars;
      repaint = true;
    }
  }
  if (repaint) {
    requestUpdate();
  }
}

void SshServerActivity::pollServerStatus() {
  if (!sshServer) {
    return;
  }
  const auto current = sshServer->getStatus();

  const bool bannerVisible = current.lastCompleteAt > 0 && (millis() - current.lastCompleteAt) < COMPLETE_BANNER_MS;
  const bool lastBannerVisible =
      lastStatus.lastCompleteAt > 0 && (millis() - lastStatus.lastCompleteAt) < COMPLETE_BANNER_MS;

  const bool stateChanged = current.clientConnected != lastStatus.clientConnected ||
                            current.inProgress != lastStatus.inProgress || current.filename != lastStatus.filename ||
                            bannerVisible != lastBannerVisible;
  // Progress-only changes are throttled: e-ink refreshes are too slow to
  // follow every 4KB chunk.
  const bool progressChanged =
      current.received != lastStatus.received && (millis() - lastProgressRepaintAt) >= PROGRESS_REPAINT_MS;
  if (stateChanged || progressChanged) {
    lastProgressRepaintAt = millis();
    requestUpdate();
  }
  lastStatus = current;
}

void SshServerActivity::render(RenderLock&&) {
  // Subactivities handle their own rendering.
  if (state != SshServerActivityState::SERVER_RUNNING && state != SshServerActivityState::AP_STARTING) {
    return;
  }

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);

  if (state == SshServerActivityState::SERVER_RUNNING) {
    renderServerRunning();
  } else {
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = (pageHeight - height) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
  }
  renderer.displayBuffer();
}

void SshServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());
  if (!isApMode) {
    renderWifiIndicator(metrics.topPadding + metrics.headerHeight);
  }

  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int height12 = renderer.getLineHeight(UI_12_FONT_ID);
  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;

  if (isApMode) {
    // Show a QR code so the client device can join the hotspot first.
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // follows spec at https://github.com/zxing/zxing/wiki/Barcode-Contents#wi-fi-network-config-android-ios-11
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      connectedSSID.c_str());
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;
  } else {
    startY += metrics.verticalSpacing * 2;
  }

  // Connection details
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_SSH_CONNECT_HINT), true,
                    EpdFontFamily::BOLD);
  startY += height10 + metrics.verticalSpacing;

  const std::string sshCommand = std::string("ssh ") + SshServer::USERNAME + "@" + connectedIP;
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, startY, sshCommand.c_str());
  startY += height12;

  const std::string sshMdns = std::string("or ssh ") + SshServer::USERNAME + "@" + AP_HOSTNAME + ".local";
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, startY, sshMdns.c_str());
  startY += height10 + metrics.verticalSpacing;

  const std::string passwordLine = std::string(tr(STR_SSH_PASSWORD)) + ": " + sessionPassword;
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, startY, passwordLine.c_str(), true, EpdFontFamily::BOLD);
  startY += height12 + metrics.verticalSpacing * 2;

  // Session / transfer status
  const auto status = sshServer ? sshServer->getStatus() : SshServer::TransferStatus{};
  if (status.inProgress && status.total > 0 && status.received <= status.total) {
    std::string label = tr(STR_SSH_RECEIVING);
    if (!status.filename.empty()) {
      label += status.filename;
      label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                     EpdFontFamily::REGULAR);
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, startY, label.c_str());
    GUI.drawProgressBar(renderer,
                        Rect{metrics.contentSidePadding, startY + height10 + metrics.verticalSpacing,
                             pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                        status.received, status.total);
  } else if (status.lastCompleteAt > 0 && (millis() - status.lastCompleteAt) < COMPLETE_BANNER_MS) {
    std::string msg = std::string(tr(STR_SSH_RECEIVED)) + status.lastCompleteName;
    msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                 EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, startY, msg.c_str());
  } else if (status.clientConnected) {
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, startY, tr(STR_SSH_CLIENT_CONNECTED));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SshServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
