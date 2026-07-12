#include "DnsLookupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void DnsLookupActivity::onEnter() {
  Activity::onEnter();
  state = TEXT_INPUT;
  resolvedIP.clear();
  resolutionFailed = false;

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DNS_LOOKUP), "example.com", 253),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          finish();
        } else {
          hostname = std::get<KeyboardResult>(result.data).text;
          if (hostname.empty()) {
            finish();
          } else {
            doResolve();
          }
        }
      });
}

void DnsLookupActivity::onExit() { Activity::onExit(); }

void DnsLookupActivity::doResolve() {
  state = RESOLVING;
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    resolutionFailed = true;
    state = RESULTS;
    requestUpdate();
    return;
  }

  IPAddress ip;
  unsigned long start = millis();
  int ret = WiFi.hostByName(hostname.c_str(), ip);
  resolutionTimeMs = millis() - start;

  if (ret == 1) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    resolvedIP = buf;
    resolutionFailed = false;
  } else {
    resolvedIP.clear();
    resolutionFailed = true;
  }

  state = RESULTS;
  requestUpdate();
}

void DnsLookupActivity::loop() {
  if (state == RESULTS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Allow new lookup
      state = TEXT_INPUT;
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DNS_LOOKUP), hostname, 253),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              finish();
            } else {
              hostname = std::get<KeyboardResult>(result.data).text;
              if (hostname.empty()) {
                finish();
              } else {
                doResolve();
              }
            }
          });
    }
  }
}

void DnsLookupActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DNS_LOOKUP));

  switch (state) {
    case TEXT_INPUT:
      break;
    case RESOLVING:
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_RESOLVING));
      break;
    case RESULTS:
      renderResults();
      break;
  }

  renderer.displayBuffer();
}

void DnsLookupActivity::renderResults() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = pageHeight / 2 - lineH * 2;

  std::string hostLine = "Host: " + hostname;
  renderer.drawCenteredText(UI_10_FONT_ID, y, hostLine.c_str());
  y += lineH + 10;

  if (resolutionFailed) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_RESOLUTION_FAILED), true, EpdFontFamily::BOLD);
  } else {
    std::string ipLine = "IP: " + resolvedIP;
    renderer.drawCenteredText(UI_12_FONT_ID, y, ipLine.c_str(), true, EpdFontFamily::BOLD);
    y += lineH + 10;

    char timeBuf[48];
    snprintf(timeBuf, sizeof(timeBuf), "Resolved in %lu ms", resolutionTimeMs);
    renderer.drawCenteredText(UI_10_FONT_ID, y, timeBuf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_AGAIN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
