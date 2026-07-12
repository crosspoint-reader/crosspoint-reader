#include "TaskManagerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <Esp.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void TaskManagerActivity::onEnter() {
  Activity::onEnter();
  state = MEMORY_VIEW;
  refreshData();
  requestUpdate();
}

void TaskManagerActivity::onExit() { Activity::onExit(); }

void TaskManagerActivity::refreshData() {
  freeHeap = esp_get_free_heap_size();
  minFreeHeap = esp_get_minimum_free_heap_size();
  largestFreeBlock = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  totalHeap = static_cast<uint32_t>(ESP.getHeapSize());
}

void TaskManagerActivity::loop() {
  unsigned long now = millis();
  if (now - lastRefresh >= REFRESH_INTERVAL_MS) {
    lastRefresh = now;
    refreshData();
    requestUpdate();
  }

  // Long-press Confirm toggles view
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 500) {
    state = (state == MEMORY_VIEW) ? SYSTEM_VIEW : MEMORY_VIEW;
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void TaskManagerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const char* title = (state == MEMORY_VIEW) ? "Task Manager" : "System Info";
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 6;
  const int x = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  char buf[72];

  if (state == MEMORY_VIEW) {
    snprintf(buf, sizeof(buf), "Free Heap:      %lu KB", (unsigned long)(freeHeap / 1024));
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "Min Free:       %lu KB", (unsigned long)(minFreeHeap / 1024));
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "Total Heap:     %lu KB", (unsigned long)(totalHeap / 1024));
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "Largest Block:  %lu KB", (unsigned long)(largestFreeBlock / 1024));
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    int fragPercent = 0;
    if (freeHeap > 0) {
      fragPercent = static_cast<int>(100 - (largestFreeBlock * 100 / freeHeap));
    }
    snprintf(buf, sizeof(buf), "Fragmentation:  %d%%", fragPercent);
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    int64_t uptimeUs = esp_timer_get_time();
    unsigned long uptimeSec = static_cast<unsigned long>(uptimeUs / 1000000LL);
    unsigned int h = uptimeSec / 3600;
    unsigned int m = (uptimeSec % 3600) / 60;
    unsigned int s = uptimeSec % 60;
    snprintf(buf, sizeof(buf), "Uptime:         %02u:%02u:%02u", h, m, s);
    renderer.drawText(UI_10_FONT_ID, x, y, buf);

  } else {
    snprintf(buf, sizeof(buf), "Flash:     %lu MB", (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)));
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "Flash Spd: %lu MHz", (unsigned long)(ESP.getFlashChipSpeed() / 1000000UL));
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "CPU:       %u MHz", (unsigned int)ESP.getCpuFreqMHz());
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "Chip:      %s  Rev %u", ESP.getChipModel(), (unsigned int)ESP.getChipRevision());
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "SD:        %s", Storage.ready() ? "Mounted" : "Not available");
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
    y += lineH;

    snprintf(buf, sizeof(buf), "Screen:    800x480 e-ink SSD1677");
    renderer.drawText(UI_10_FONT_ID, x, y, buf);
  }

  const char* toggleHint = (state == MEMORY_VIEW) ? "System" : "Memory";
  char hintBuf[32];
  snprintf(hintBuf, sizeof(hintBuf), "Hold=%s", toggleHint);
  GUI.drawButtonHints(renderer, "Back", hintBuf, "", "");

  renderer.displayBuffer();
}
