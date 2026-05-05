#include "ClockActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <ctime>

#include "MappedInputManager.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Tick task — fires once per minute aligned to the wall-clock boundary.
// ---------------------------------------------------------------------------

void ClockActivity::tickTaskTrampoline(void* param) {
  static_cast<ClockActivity*>(param)->tickTask();
}

[[noreturn]] void ClockActivity::tickTask() {
  while (true) {
    // Sleep until the top of the next minute.
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    const int secsUntilNextMinute = 60 - static_cast<int>(tv.tv_sec % 60);
    vTaskDelay(pdMS_TO_TICKS(secsUntilNextMinute * 1000UL));

    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Activity lifecycle
// ---------------------------------------------------------------------------

void ClockActivity::onEnter() {
  Activity::onEnter();

  xTaskCreate(tickTaskTrampoline, "ClockTick",
              2048,           // stack bytes — simple task, no heap alloc
              this, 1, &tickTaskHandle);

  requestUpdate();
}

void ClockActivity::onExit() {
  if (tickTaskHandle) {
    vTaskDelete(tickTaskHandle);
    tickTaskHandle = nullptr;
  }
  Activity::onExit();
}

void ClockActivity::loop() {
  mappedInput.update();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void ClockActivity::render(RenderLock&&) {
  rendersSinceFullRefresh++;
  const bool doFullRefresh = (rendersSinceFullRefresh >= FULL_REFRESH_INTERVAL);
  if (doFullRefresh) {
    rendersSinceFullRefresh = 0;
  }
  renderClock(doFullRefresh);
}

void ClockActivity::renderClock(bool fullRefresh) {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  struct tm now {};
  localtime_r(&tv.tv_sec, &now);

  char timeBuf[6];  // "HH:MM\0"
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", now.tm_hour, now.tm_min);

  char dateBuf[12];  // "Mon 05 May\0" — enough for any locale
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
           now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);

  renderer.clearScreen();

  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();

  // Large HH:MM in the centre
  const int timeLineH = renderer.getLineHeight(NOTOSERIF_18_FONT_ID);
  const int timeY = (sh - timeLineH) / 2 - timeLineH / 2;
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, timeY, timeBuf);

  // Smaller date below
  const int dateLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int dateY = timeY + timeLineH + dateLineH / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, dateY, dateBuf);

  // Button hint at the bottom
  const int hintLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int hintY = sh - hintLineH - 8;
  renderer.drawCenteredText(UI_10_FONT_ID, hintY, tr(STR_CLOCK_BACK));

  const HalDisplay::RefreshMode mode = fullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
  renderer.displayBuffer(mode);

  LOG_DBG("CLK", "Rendered %s (full=%d, heap=%d)", timeBuf, fullRefresh, ESP.getFreeHeap());
}
