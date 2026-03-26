#include "SyncTimeActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

void SyncTimeActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onWifiSelectionCancelled();
                             return;
                           }
                           onWifiSelectionComplete(true);
                         });
}

void SyncTimeActivity::onExit() {
  Activity::onExit();
  wifiOff();
}

void SyncTimeActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    state = FAILED;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = SYNCING;
  }
  requestUpdateAndWait();

  performSync();
}

void SyncTimeActivity::onWifiSelectionCancelled() { finish(); }

void SyncTimeActivity::performSync() {
  bool ok = HalClock::syncNtp();
  wifiOff();

  state = ok ? SUCCESS : FAILED;
  requestUpdate();
}

void SyncTimeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_TIME));

  if (state == SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SYNCING_CLOCK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_TIME_SYNCED), true, EpdFontFamily::BOLD);

    time_t now = HalClock::now();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char timePart[16];
    HalClock::formatTime(timePart, sizeof(timePart), !SETTINGS.clockFormat12h);
    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%s  %04d-%02d-%02d", timePart, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
             timeinfo.tm_mday);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, timeStr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_TIME_SYNC_FAILED), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void SyncTimeActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}
