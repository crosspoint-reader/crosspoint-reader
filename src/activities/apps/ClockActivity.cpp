#include "ClockActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <time.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ClockActivity::onEnter() {
  Activity::onEnter();

  // Reset all mode state
  clockMode = NTP_CLOCK;

  // NTP state
  ntpSynced = false;
  ntpSyncFailed = false;
  ntpLastUpdateMs = 0;

  // Stopwatch state
  swState = SW_IDLE;
  swElapsedMs = 0;
  swPausedElapsed = 0;
  swStartMs = 0;
  swLaps.clear();

  // Pomodoro state
  pomState = POM_IDLE;
  pomPaused = false;
  pomRemainingMs = pomWorkDurationMs;
  pomLastTickMs = millis();
  pomCompletedPomodoros = 0;
  pomCycleCount = 0;

  lastDisplayMs = 0;

  // Attempt NTP sync (WiFi connection handled via sub-activity if needed)
  ntpSync();
}

void ClockActivity::onExit() { Activity::onExit(); }

void ClockActivity::ntpSync() {
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               ntpSyncFailed = true;
                               requestUpdate();
                             } else {
                               configTime(0, 0, "pool.ntp.org", "time.nist.gov");
                               struct tm timeinfo;
                               if (getLocalTime(&timeinfo, 10000)) {
                                 ntpSynced = true;
                               } else {
                                 ntpSyncFailed = true;
                               }
                               ntpLastUpdateMs = millis();
                               requestUpdate();
                             }
                           });
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    ntpSynced = true;
  } else {
    ntpSyncFailed = true;
  }
  ntpLastUpdateMs = millis();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

const char* ClockActivity::dayOfWeekName(int wday) {
  static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  if (wday >= 0 && wday <= 6) return days[wday];
  return "???";
}

const char* ClockActivity::monthName(int mon) {
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (mon >= 0 && mon <= 11) return months[mon];
  return "???";
}

void ClockActivity::formatMs(unsigned long ms, char* buf, size_t len) {
  unsigned long totalSec = ms / 1000;
  int m = totalSec / 60;
  int s = totalSec % 60;
  int cs = (ms % 1000) / 10;
  snprintf(buf, len, "%02d:%02d.%02d", m, s, cs);
}

void ClockActivity::formatTime(unsigned long ms, char* buf, size_t bufLen) {
  unsigned long totalSec = ms / 1000;
  int h = totalSec / 3600;
  int m = (totalSec % 3600) / 60;
  int s = totalSec % 60;
  snprintf(buf, bufLen, "%02d:%02d:%02d", h, m, s);
}

const char* ClockActivity::pomStateLabel() const {
  switch (pomState) {
    case POM_IDLE:
      return "Ready";
    case POM_WORK:
      return pomPaused ? "Work (Paused)" : "Work";
    case POM_SHORT_BREAK:
      return pomPaused ? "Short Break (Paused)" : "Short Break";
    case POM_LONG_BREAK:
      return pomPaused ? "Long Break (Paused)" : "Long Break";
  }
  return "";
}

// ---------------------------------------------------------------------------
// Pomodoro helpers
// ---------------------------------------------------------------------------

void ClockActivity::pomStartWork() {
  pomState = POM_WORK;
  pomPaused = false;
  pomRemainingMs = pomWorkDurationMs;
  pomLastTickMs = millis();
  requestUpdate();
}

void ClockActivity::pomStartShortBreak() {
  pomState = POM_SHORT_BREAK;
  pomPaused = false;
  pomRemainingMs = pomShortBreakMs;
  pomLastTickMs = millis();
  requestUpdate();
}

void ClockActivity::pomStartLongBreak() {
  pomState = POM_LONG_BREAK;
  pomPaused = false;
  pomRemainingMs = pomLongBreakMs;
  pomLastTickMs = millis();
  requestUpdate();
}

void ClockActivity::pomResetToIdle() {
  pomState = POM_IDLE;
  pomPaused = false;
  pomRemainingMs = pomWorkDurationMs;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void ClockActivity::loop() {
  unsigned long now = millis();

  // Long-press Confirm cycles modes (checked on release)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 500) {
    switch (clockMode) {
      case NTP_CLOCK:
        clockMode = STOPWATCH_MODE;
        break;
      case STOPWATCH_MODE:
        clockMode = POMODORO_MODE;
        break;
      case POMODORO_MODE:
        clockMode = NTP_CLOCK;
        break;
    }
    requestUpdate();
    return;
  }

  // Per-mode logic
  switch (clockMode) {
    case NTP_CLOCK: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      if (ntpSynced) {
        if (now - ntpLastUpdateMs >= NTP_UPDATE_INTERVAL_MS) {
          ntpLastUpdateMs = now;
          requestUpdate();
        }
      }
      break;
    }

    case STOPWATCH_MODE: {
      if (swState == SW_RUNNING) {
        swElapsedMs = swPausedElapsed + (now - swStartMs);
        if (now - lastDisplayMs >= DISPLAY_INTERVAL) {
          lastDisplayMs = now;
          requestUpdate();
        }
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (swState == SW_IDLE) {
          swStartMs = now;
          swPausedElapsed = 0;
          swState = SW_RUNNING;
        } else if (swState == SW_RUNNING) {
          // Lap
          if (static_cast<int>(swLaps.size()) < MAX_LAPS) {
            swLaps.push_back(swElapsedMs);
          }
        } else {
          // Stopped -> restart
          swStartMs = now;
          swPausedElapsed = 0;
          swElapsedMs = 0;
          swLaps.clear();
          swState = SW_RUNNING;
        }
        requestUpdate();
        return;
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        if (swState == SW_RUNNING) {
          swPausedElapsed = swElapsedMs;
          swState = SW_STOPPED;
          requestUpdate();
        } else {
          // Idle or stopped: exit activity
          finish();
        }
        return;
      }
      break;
    }

    case POMODORO_MODE: {
      // Timer tick
      if (!pomPaused && pomState != POM_IDLE) {
        unsigned long elapsed = now - pomLastTickMs;
        pomLastTickMs = now;

        if (elapsed >= pomRemainingMs) {
          pomRemainingMs = 0;
        } else {
          pomRemainingMs -= elapsed;
        }

        if (pomRemainingMs == 0) {
          if (pomState == POM_WORK) {
            pomCompletedPomodoros++;
            pomCycleCount++;
            if (pomCycleCount >= 4) {
              pomCycleCount = 0;
              pomStartLongBreak();
            } else {
              pomStartShortBreak();
            }
            return;
          } else {
            pomStartWork();
            return;
          }
        }

        if (now - lastDisplayMs >= POM_DISPLAY_INTERVAL) {
          lastDisplayMs = now;
          requestUpdate();
        }
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (pomState == POM_IDLE) {
          pomStartWork();
        } else {
          pomPaused = !pomPaused;
          if (!pomPaused) {
            pomLastTickMs = millis();
          }
          requestUpdate();
        }
        return;
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        if (pomState == POM_IDLE) {
          finish();
        } else {
          pomResetToIdle();
        }
        return;
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        pomRemainingMs += 60000;
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        if (pomRemainingMs > 60000) {
          pomRemainingMs -= 60000;
        } else {
          pomRemainingMs = 0;
        }
        requestUpdate();
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void ClockActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (clockMode) {
    case NTP_CLOCK:
      renderNtpClock();
      break;
    case STOPWATCH_MODE:
      renderStopwatch();
      break;
    case POMODORO_MODE:
      renderPomodoro();
      break;
  }

  renderer.displayBuffer();
}

void ClockActivity::renderNtpClock() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Clock");

  if (ntpSyncFailed) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_NTP_SYNC_FAILED), true, EpdFontFamily::BOLD);
  } else if (!ntpSynced) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SYNCING_NTP));
  } else {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int y = pageHeight / 2 - 60;

      renderer.drawCenteredText(UI_10_FONT_ID, y, dayOfWeekName(timeinfo.tm_wday));
      y += 40;

      char timeBuf[16];
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      renderer.drawCenteredText(UI_12_FONT_ID, y, timeBuf, true, EpdFontFamily::BOLD);
      y += 50;

      char dateBuf[32];
      snprintf(dateBuf, sizeof(dateBuf), "%s %d, %d", monthName(timeinfo.tm_mon), timeinfo.tm_mday,
               timeinfo.tm_year + 1900);
      renderer.drawCenteredText(UI_10_FONT_ID, y, dateBuf);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "HoldConfirm=Mode", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ClockActivity::renderStopwatch() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Stopwatch");

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  char timeBuf[16];
  formatMs(swElapsedMs, timeBuf, sizeof(timeBuf));
  renderer.drawCenteredText(UI_12_FONT_ID, y, timeBuf, true, EpdFontFamily::BOLD);
  y += 45;

  for (int i = static_cast<int>(swLaps.size()) - 1;
       i >= 0 && y < pageHeight - metrics.buttonHintsHeight - 20; i--) {
    char lapBuf[32];
    char lapTime[16];
    formatMs(swLaps[i], lapTime, sizeof(lapTime));
    snprintf(lapBuf, sizeof(lapBuf), "Lap %d: %s", i + 1, lapTime);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, lapBuf);
    y += lineH;
  }

  const char* confirmLabel =
      (swState == SW_IDLE) ? "Start" : (swState == SW_RUNNING ? "Lap" : "Restart");
  const char* backLabel = (swState == SW_RUNNING) ? "Stop" : tr(STR_BACK);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ClockActivity::renderPomodoro() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Pomodoro");

  int y = pageHeight / 2 - 80;
  renderer.drawCenteredText(UI_12_FONT_ID, y, pomStateLabel(), true, EpdFontFamily::BOLD);

  char timeBuf[16];
  formatTime(pomRemainingMs, timeBuf, sizeof(timeBuf));
  y += 50;
  renderer.drawCenteredText(UI_12_FONT_ID, y, timeBuf, true, EpdFontFamily::BOLD);

  y += 50;
  char countBuf[48];
  snprintf(countBuf, sizeof(countBuf), "Completed: %d  Cycle: %d/4", pomCompletedPomodoros, pomCycleCount);
  renderer.drawCenteredText(UI_10_FONT_ID, y, countBuf);

  const char* confirmLabel = (pomState == POM_IDLE) ? "Start" : (pomPaused ? "Resume" : "Pause");
  const char* backLabel = (pomState == POM_IDLE) ? tr(STR_BACK) : "Reset";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "+1min", "-1min");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
