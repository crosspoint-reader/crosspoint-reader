#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <HalSystem.h>  // feedWatchdog()
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/SignalStrengthWidget.h"
#include "components/TouchExitButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";
}  // namespace

// Unlike CrossPointWebServerActivity we keep rendering live progress UI, so the primary
// framebuffer must stay. The ~52 KB secondary buffer, the glyph cache and the SD reader font
// are unused here though. Safe to drop without restoring: onExit() always silentRestart()s.
//
// Idempotent, and called both before the radio comes up and again before the server starts.
void CalibreConnectActivity::freeMemoryBeforeRadio() {
  LOG_DBG("CAL", "Free heap before trim: %d bytes", ESP.getFreeHeap());
  sdFontSystem.unload(renderer);
  trimMemoryForNetworkSession(renderer, "CAL");
  LOG_DBG("CAL", "Free heap after trim: %d bytes", ESP.getFreeHeap());
}

void CalibreConnectActivity::onEnter() {
  Activity::onEnter();

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
  lastProcessedCompleteAt = 0;
  lastDeleteName.clear();
  lastDeleteCount = 0;
  lastDeleteAt = 0;
  lastProcessedDeleteAt = 0;
  currentRssi = 0;
  lastRssiUpdateTime = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
    // BEFORE the radio, not after the join: bringing up the WiFi stack is itself
    // allocation-heavy, and on a lean heap it is the association that fails.
    freeMemoryBeforeRadio();
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
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  Activity::onExit();

  wifiWatch.stop();  // before the disconnect below, a drop of our own making
  stopWebServer();
  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }

  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  state = CalibreConnectState::SERVER_STARTING;
  requestUpdate();

  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  // The trim itself already happened in freeMemoryBeforeRadio(), before the join. Repeated here
  // because it is idempotent and the status screens drawn since then have repopulated the glyph
  // cache, which the web server would rather have as free heap (each request wants an 8 KB
  // contiguous block).
  freeMemoryBeforeRadio();

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    wifiWatch.start();
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
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || TouchExitButton::tapped(mappedInput)) {
    exitRequested = true;
  }

  if (webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("CAL", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    HalSystem::feedWatchdog();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) {
        HalSystem::feedWatchdog();
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

    // A drop used to go unnoticed here: the screen kept its last signal reading while the plugin
    // could no longer reach the reader. Now it says the link is reconnecting, then that it failed
    // (after WifiLinkWatch::GRACE_MS), and returns to normal if the driver gets it back. Back
    // leaves at any time.
    const auto link = wifiWatch.poll(connectedIP);
    if (link != shownLink) {
      shownLink = link;
      if (link != WifiLinkWatch::Link::Up) currentRssi = 0;
      requestUpdate();
    }

    if (shownLink == WifiLinkWatch::Link::Up && millis() - lastRssiUpdateTime > 5000) {
      lastRssiUpdateTime = millis();
      currentRssi = WiFi.RSSI();
      requestUpdate();
    }

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
    // Only update lastCompleteAt if the server has a NEW value (not one we already processed)
    // This prevents restoring an old value after the 6s timeout clears it
    if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastProcessedCompleteAt) {
      lastCompleteAt = status.lastCompleteAt;
      lastCompleteName = status.lastCompleteName;
      lastProcessedCompleteAt = status.lastCompleteAt;  // Mark this value as processed
      changed = true;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      // Note: we DON'T reset lastProcessedCompleteAt here, so we won't re-process the old server value
      changed = true;
    }
    // Same pattern for deletions (HTTP /delete requests from the Calibre plugin)
    if (status.lastDeleteAt != 0 && status.lastDeleteAt != lastProcessedDeleteAt) {
      lastDeleteAt = status.lastDeleteAt;
      lastDeleteName = status.lastDeleteName;
      lastDeleteCount = status.lastDeleteCount;
      lastProcessedDeleteAt = status.lastDeleteAt;
      changed = true;
    }
    if (lastDeleteAt > 0 && (millis() - lastDeleteAt) >= 6000) {
      lastDeleteAt = 0;
      lastDeleteName.clear();
      changed = true;
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    finish();
    return;
  }
}

void CalibreConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_CALIBRE_WIRELESS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = contentRect.y + (contentRect.height - height) / 2;

  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CALIBRE_STARTING));
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    GUI.drawSubHeader(
        renderer,
        Rect{contentRect.x, metrics.topPadding + metrics.headerHeight, contentRect.width, metrics.tabBarHeight},
        connectedSSID.c_str(), (std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP).c_str());

    const int textX = contentRect.x + metrics.contentSidePadding;
    const int textWidth = contentRect.width - metrics.contentSidePadding * 2;
    int y = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
    const auto heightText12 = renderer.getTextHeight(UI_12_FONT_ID);
    const auto lineHeightSmall = renderer.getLineHeight(SMALL_FONT_ID);

    const int signalHeight = 22;
    drawWifiSignalStrength(renderer, textX, y, textWidth, signalHeight, currentRssi);
    const std::string signalText = shownLink == WifiLinkWatch::Link::Reconnecting ? tr(STR_WIFI_RECONNECTING)
                                   : shownLink == WifiLinkWatch::Link::Gone       ? tr(STR_WIFI_CONN_FAILED)
                                                                                  : rssiLabel(currentRssi);
    renderer.drawText(SMALL_FONT_ID, textX, y + signalHeight + 2, signalText.c_str());
    y += signalHeight + lineHeightSmall + metrics.verticalSpacing * 3;
    const int belowExit = TouchExitButton::draw(renderer, mappedInput, textX, y, textWidth);
    if (belowExit != y) y = belowExit + metrics.verticalSpacing * 3;

    renderer.drawText(UI_12_FONT_ID, textX, y, tr(STR_CALIBRE_SETUP), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    renderer.drawText(SMALL_FONT_ID, textX, y, tr(STR_CALIBRE_INSTRUCTION_1));
    renderer.drawText(SMALL_FONT_ID, textX, y + height, tr(STR_CALIBRE_INSTRUCTION_2));
    renderer.drawText(SMALL_FONT_ID, textX, y + height * 2, tr(STR_CALIBRE_INSTRUCTION_3));
    renderer.drawText(SMALL_FONT_ID, textX, y + height * 3, tr(STR_CALIBRE_INSTRUCTION_4));

    y += height * 3 + metrics.verticalSpacing * 4;
    renderer.drawText(UI_12_FONT_ID, textX, y, tr(STR_CALIBRE_STATUS), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!currentUploadName.empty()) {
        label += ": " + currentUploadName;
        label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), textWidth, EpdFontFamily::REGULAR);
      }
      renderer.drawText(SMALL_FONT_ID, textX, y, label.c_str());
      GUI.drawProgressBar(renderer,
                          Rect{contentRect.x + metrics.contentSidePadding, y + height + metrics.verticalSpacing,
                               textWidth, metrics.progressBarHeight},
                          lastProgressReceived, lastProgressTotal);
      y += height + metrics.verticalSpacing * 2 + metrics.progressBarHeight;
    }

    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), textWidth, EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, textX, y, msg.c_str());
      y += height;
    }

    if (lastDeleteAt > 0 && (millis() - lastDeleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_DELETED)) + lastDeleteName;
      if (lastDeleteCount > 1) {
        msg += " (+" + std::to_string(lastDeleteCount - 1) + ")";
      }
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), textWidth, EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, textX, y, msg.c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
