#include "SyncTimeActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

static void formatDuration(char* buf, size_t bufSize, int32_t totalSeconds) {
  const char* sign = totalSeconds < 0 ? "-" : "+";
  int32_t abs = totalSeconds < 0 ? -totalSeconds : totalSeconds;

  int32_t days = abs / 86400;
  int32_t hours = (abs % 86400) / 3600;
  int32_t mins = (abs % 3600) / 60;
  int32_t secs = abs % 60;

  if (days > 0) {
    snprintf(buf, bufSize, "%s%ldd %ldh %ldm", sign, (long)days, (long)hours, (long)mins);
  } else if (hours > 0) {
    snprintf(buf, bufSize, "%s%ldh %ldm %lds", sign, (long)hours, (long)mins, (long)secs);
  } else if (mins > 0) {
    snprintf(buf, bufSize, "%s%ldm %lds", sign, (long)mins, (long)secs);
  } else {
    snprintf(buf, bufSize, "%s%lds", sign, (long)secs);
  }
}

static void formatElapsed(char* buf, size_t bufSize, int32_t totalSeconds) {
  int32_t days = totalSeconds / 86400;
  int32_t hours = (totalSeconds % 86400) / 3600;
  int32_t mins = (totalSeconds % 3600) / 60;

  if (days > 0) {
    snprintf(buf, bufSize, "%ldd %ldh ago", (long)days, (long)hours);
  } else if (hours > 0) {
    snprintf(buf, bufSize, "%ldh %ldm ago", (long)hours, (long)mins);
  } else {
    snprintf(buf, bufSize, "%ldm ago", (long)mins);
  }
}

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
  HalClock::wifiOff(true);
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
  hadTimeBeforeSync = HalClock::isSynced();
  preSyncTime = hadTimeBeforeSync ? time(nullptr) : 0;
  prevSyncTime = HalClock::lastSyncTime();

  bool ok = HalClock::syncNtp();

  if (ok && hadTimeBeforeSync) {
    driftSeconds = (int32_t)(time(nullptr) - preSyncTime);
  }

  HalClock::wifiOff(true);

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
    int y = pageHeight / 2 - 40;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TIME_SYNCED), true, EpdFontFamily::BOLD);

    time_t now = HalClock::now();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char timePart[16];
    HalClock::formatTime(timePart, sizeof(timePart), !SETTINGS.clockFormat12h);
    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%s  %04d-%02d-%02d", timePart, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
             timeinfo.tm_mday);
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, timeStr);

    int32_t elapsedSinceSync = -1;
    if (prevSyncTime > 0) {
      elapsedSinceSync = (int32_t)(now - prevSyncTime);
    }

    char driftStr[80];
    if (hadTimeBeforeSync) {
      char driftFmt[24];
      formatDuration(driftFmt, sizeof(driftFmt), driftSeconds);

      if (elapsedSinceSync > 0) {
        double hours = (double)elapsedSinceSync / 3600.0;
        double rate = (double)driftSeconds / hours;
        char rateFmt[16];
        snprintf(rateFmt, sizeof(rateFmt), "%+.2f", rate);

        char driftWithRate[48];
        snprintf(driftWithRate, sizeof(driftWithRate), "%s (%s s/hr)", driftFmt, rateFmt);
        snprintf(driftStr, sizeof(driftStr), tr(STR_CLOCK_DRIFT), driftWithRate);
      } else {
        snprintf(driftStr, sizeof(driftStr), tr(STR_CLOCK_DRIFT), driftFmt);
      }
    } else {
      snprintf(driftStr, sizeof(driftStr), tr(STR_CLOCK_DRIFT), "N/A");
    }
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, driftStr);

    if (elapsedSinceSync > 0) {
      char elapsedFmt[24];
      formatElapsed(elapsedFmt, sizeof(elapsedFmt), elapsedSinceSync);
      char lastSyncStr[48];
      snprintf(lastSyncStr, sizeof(lastSyncStr), tr(STR_LAST_NTP_SYNC), elapsedFmt);
      y += 25;
      renderer.drawCenteredText(UI_10_FONT_ID, y, lastSyncStr);
    }

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
