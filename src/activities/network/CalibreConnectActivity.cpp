#include "CalibreConnectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "WifiSelectionActivity.h"
#include "activities/network/SignalStrengthWidget.h"
#include "components/TouchExitButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Unlike CrossPointWebServerActivity we keep rendering live progress UI, so the primary
// framebuffer stays; ServerSession::prepareRadio() drops only what this screen does not use.
void CalibreConnectActivity::onEnter() {
  Activity::onEnter();

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
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
  shownLink = WifiLinkWatch::Link::Up;

  if (WiFi.status() != WL_CONNECTED) {
    // BEFORE the radio, not after the join: bringing up the WiFi stack is itself
    // allocation-heavy, and on a lean heap it is the association that fails.
    session.prepareRadio();
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
  session.end();
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

  // The trim already happened before the join. Repeated because the status screens drawn since
  // have repopulated the glyph cache, which the web server would rather have as free heap (each
  // request wants an 8 KB contiguous block).
  session.prepareRadio();
  state = session.start(false) ? CalibreConnectState::SERVER_RUNNING : CalibreConnectState::ERROR;
  requestUpdate();
}

void CalibreConnectActivity::loop() {
  // Fewer rounds per pass than File Transfer: this screen shows the transfer's progress, which the
  // code below keeps up to date between passes.
  if (!session.serve(80)) {
    finish();
    return;
  }

  if (session.running()) {
    // A drop used to go unnoticed here: the screen kept its last signal reading while the plugin
    // could no longer reach the reader. Now it says the link is reconnecting, then that it failed
    // (after WifiLinkWatch::GRACE_MS), and returns to normal if the driver gets it back. Back
    // leaves at any time.
    const auto link = session.link(connectedIP);
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

    const auto status = session.webServer()->getWsUploadStatus();
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
