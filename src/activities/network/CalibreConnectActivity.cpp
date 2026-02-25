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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CALIBRE_WIRELESS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CALIBRE_STARTING));
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                      connectedSSID.c_str(), (std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP).c_str());

    int y = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 4;
    const auto heightText12 = renderer.getTextHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_SETUP), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_INSTRUCTION_1));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height, tr(STR_CALIBRE_INSTRUCTION_2));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height * 2, tr(STR_CALIBRE_INSTRUCTION_3));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height * 3, tr(STR_CALIBRE_INSTRUCTION_4));

    y += height * 3 + metrics.verticalSpacing * 4;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_STATUS), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!currentUploadName.empty()) {
        label += ": " + currentUploadName;
        label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                       EpdFontFamily::REGULAR);
      }
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, label.c_str());
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, y + height + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          lastProgressReceived, lastProgressTotal);
      y += height + metrics.verticalSpacing * 2 + metrics.progressBarHeight;
    }

    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                   EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, msg.c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
