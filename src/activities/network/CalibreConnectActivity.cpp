#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";
}  // namespace

void CalibreConnectActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  lastProgressReceived = 0;
  lastProgressTotal = 0;
  currentUploadName.clear();
  lastCompleteName.clear();
  lastCompleteAt = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  ActivityWithSubactivity::onExit();

  stopWebServer();
  MDNS.end();

  delay(50);
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    exitActivity();
    onComplete();
    return;
  }

  if (subActivity) {
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
  }
  connectedSSID = WiFi.SSID().c_str();
  exitActivity();
  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  state = CalibreConnectState::SERVER_STARTING;
  requestUpdate();

  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    requestUpdate();
  } else {
    state = CalibreConnectState::ERROR;
    requestUpdate();
  }
}

void CalibreConnectActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  if (webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("CAL", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    esp_task_wdt_reset();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) {
        esp_task_wdt_reset();
      }
      if ((i & 0x0F) == 0x0F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          exitRequested = true;
          break;
        }
      }
    }
    lastHandleClientTime = millis();

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
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      changed = true;
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    onComplete();
    return;
  }
}

void CalibreConnectActivity::render(Activity::RenderLock&&) {
  if (state == CalibreConnectState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  const auto pageHeight = renderer.getScreenHeight();
  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_CALIBRE_STARTING), true, EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  }
  renderer.displayBuffer();
}

void CalibreConnectActivity::renderServerRunning() const {
  constexpr int LINE_SPACING = 24;
  constexpr int SMALL_SPACING = 20;
  constexpr int SECTION_SPACING = 40;
  constexpr int TOP_PADDING = 14;
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_CALIBRE_WIRELESS), true, EpdFontFamily::BOLD);

  int y = 55 + TOP_PADDING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_WIFI_NETWORKS), true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + connectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, ssidInfo.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, y + LINE_SPACING,
                            (std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP).c_str());

  y += LINE_SPACING * 2 + SECTION_SPACING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CALIBRE_SETUP), true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_CALIBRE_INSTRUCTION_1));
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING, tr(STR_CALIBRE_INSTRUCTION_2));
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING * 2, tr(STR_CALIBRE_INSTRUCTION_3));
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING * 3, tr(STR_CALIBRE_INSTRUCTION_4));

  y += SMALL_SPACING * 3 + SECTION_SPACING;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CALIBRE_STATUS), true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
    std::string label = tr(STR_CALIBRE_RECEIVING);
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
    GUI.drawProgressBar(renderer, Rect{barX, y + 22, barWidth, barHeight}, lastProgressReceived, lastProgressTotal);
    y += 40;
  }

  if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
    std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
    if (msg.length() > 36) {
      msg.replace(33, msg.length() - 33, "...");
    }
    renderer.drawCenteredText(SMALL_FONT_ID, y, msg.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
