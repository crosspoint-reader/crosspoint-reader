#include "PingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PingActivity::onEnter() {
  Activity::onEnter();
  state = TEXT_INPUT;
  results.clear();
  totalSent = 0;
  totalSuccess = 0;
  minRtt = 0;
  maxRtt = 0;
  totalRtt = 0;

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Ping Host/IP", "8.8.8.8", 253),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          finish();
        } else {
          targetHost = std::get<KeyboardResult>(result.data).text;
          if (targetHost.empty()) {
            finish();
          } else {
            state = PINGING;
            lastPingTime = 0;
          }
        }
      });
}

void PingActivity::onExit() { Activity::onExit(); }

void PingActivity::doPing() {
  if (WiFi.status() != WL_CONNECTED) {
    PingResult pr;
    pr.success = false;
    pr.rttMs = 0;
    results.push_back(pr);
    if (static_cast<int>(results.size()) > MAX_DISPLAY_RESULTS) {
      results.erase(results.begin());
    }
    totalSent++;
    return;
  }

  WiFiClient client;
  unsigned long start = millis();
  bool ok = client.connect(targetHost.c_str(), 80, 3000);
  unsigned long rtt = millis() - start;
  client.stop();

  PingResult pr;
  pr.success = ok;
  pr.rttMs = ok ? rtt : 0;

  results.push_back(pr);
  if (static_cast<int>(results.size()) > MAX_DISPLAY_RESULTS) {
    results.erase(results.begin());
  }

  totalSent++;
  if (ok) {
    totalSuccess++;
    totalRtt += rtt;
    if (totalSuccess == 1) {
      minRtt = rtt;
      maxRtt = rtt;
    } else {
      if (rtt < minRtt) minRtt = rtt;
      if (rtt > maxRtt) maxRtt = rtt;
    }
  }
}

void PingActivity::loop() {
  if (state == PINGING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = SUMMARY;
      requestUpdate();
      return;
    }

    unsigned long now = millis();
    if (now - lastPingTime >= PING_INTERVAL_MS) {
      lastPingTime = now;
      doPing();
      requestUpdate();
    }
    return;
  }

  if (state == SUMMARY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void PingActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PING_TOOL));

  switch (state) {
    case TEXT_INPUT:
      break;
    case PINGING:
      renderPinging();
      break;
    case SUMMARY:
      renderSummary();
      break;
  }

  renderer.displayBuffer();
}

void PingActivity::renderPinging() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  // Show target
  std::string hostLine = "Target: " + targetHost;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, hostLine.c_str());
  y += lineH + 5;

  // Show recent results
  for (auto& r : results) {
    char buf[64];
    if (r.success) {
      snprintf(buf, sizeof(buf), "Reply: %lu ms", r.rttMs);
    } else {
      snprintf(buf, sizeof(buf), "Timeout");
    }
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, buf);
    y += lineH;
  }

  y += 10;
  // Stats line
  if (totalSent > 0) {
    int lossPercent = ((totalSent - totalSuccess) * 100) / totalSent;
    unsigned long avgRtt = totalSuccess > 0 ? totalRtt / totalSuccess : 0;
    char statsBuf[128];
    snprintf(statsBuf, sizeof(statsBuf), "Sent:%d OK:%d Loss:%d%%", totalSent, totalSuccess, lossPercent);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, statsBuf);
    y += lineH;

    if (totalSuccess > 0) {
      char rttBuf[80];
      snprintf(rttBuf, sizeof(rttBuf), "Min:%lu Avg:%lu Max:%lu ms", minRtt, avgRtt, maxRtt);
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, rttBuf);
    }
  }

  const auto labels = mappedInput.mapLabels("Summary", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PingActivity::renderSummary() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = pageHeight / 2 - lineH * 3;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_PING_SUMMARY), true, EpdFontFamily::BOLD);
  y += 40;

  std::string hostLine = "Host: " + targetHost;
  renderer.drawCenteredText(UI_10_FONT_ID, y, hostLine.c_str());
  y += lineH + 5;

  char buf[80];
  snprintf(buf, sizeof(buf), "Sent: %d  Received: %d", totalSent, totalSuccess);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += lineH + 5;

  if (totalSent > 0) {
    int loss = ((totalSent - totalSuccess) * 100) / totalSent;
    snprintf(buf, sizeof(buf), "Packet Loss: %d%%", loss);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
    y += lineH + 5;
  }

  if (totalSuccess > 0) {
    unsigned long avg = totalRtt / totalSuccess;
    snprintf(buf, sizeof(buf), "RTT min/avg/max: %lu/%lu/%lu ms", minRtt, avg, maxRtt);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
