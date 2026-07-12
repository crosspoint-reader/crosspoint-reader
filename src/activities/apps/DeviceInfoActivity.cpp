#include "DeviceInfoActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <Esp.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// CROSSPOINT_VERSION is a build-time define, provide fallback if missing
#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

void DeviceInfoActivity::onEnter() {
  Activity::onEnter();
  scrollOffset = 0;
  requestUpdate();
}

void DeviceInfoActivity::onExit() { Activity::onExit(); }

void DeviceInfoActivity::loop() {
  buttonNavigator.onNext([this] {
    scrollOffset++;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    if (scrollOffset > 0) scrollOffset--;
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void DeviceInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Device Info");

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int x = metrics.contentSidePadding;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int visibleLines = (contentBottom - contentTop) / lineH;

  // Build all info lines into fixed-size char arrays to avoid heap strings
  char lines[LINE_COUNT][80];
  int n = 0;

  // 0: Firmware
  snprintf(lines[n++], 80, "Firmware:  %s", CROSSPOINT_VERSION);

  // 1: Chip
  snprintf(lines[n++], 80, "Chip:      %s  Rev %u", ESP.getChipModel(), (unsigned)ESP.getChipRevision());

  // 2: CPU
  snprintf(lines[n++], 80, "CPU:       %u MHz", (unsigned)ESP.getCpuFreqMHz());

  // 3: Flash
  snprintf(lines[n++], 80, "Flash:     %lu MB", (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)));

  // 4: Free Heap
  snprintf(lines[n++], 80, "Free Heap: %lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));

  // 5: Uptime
  {
    int64_t uptimeUs = esp_timer_get_time();
    unsigned long uptimeSec = static_cast<unsigned long>(uptimeUs / 1000000LL);
    unsigned int h = uptimeSec / 3600;
    unsigned int m = (uptimeSec % 3600) / 60;
    unsigned int s = uptimeSec % 60;
    snprintf(lines[n++], 80, "Uptime:    %02u:%02u:%02u", h, m, s);
  }

  // 6: WiFi
  {
    if (WiFi.status() == WL_CONNECTED) {
      char ipBuf[20];
      WiFi.localIP().toString().toCharArray(ipBuf, sizeof(ipBuf));
      snprintf(lines[n++], 80, "WiFi:      %s  %s", WiFi.SSID().c_str(), ipBuf);
    } else {
      snprintf(lines[n++], 80, "WiFi:      Off");
    }
  }

  // 7: Screen
  snprintf(lines[n++], 80, "Screen:    800x480 e-ink");

  // 8: Display controller
  snprintf(lines[n++], 80, "Display:   SSD1677");

  // Clamp scroll
  int maxScroll = LINE_COUNT - visibleLines;
  if (maxScroll < 0) maxScroll = 0;
  if (scrollOffset > maxScroll) scrollOffset = maxScroll;

  // Render visible lines
  int y = contentTop;
  for (int i = scrollOffset; i < LINE_COUNT && i < scrollOffset + visibleLines; i++) {
    renderer.drawText(UI_10_FONT_ID, x, y, lines[i]);
    y += lineH;
  }

  // Scroll indicator
  if (maxScroll > 0) {
    char scrollBuf[16];
    snprintf(scrollBuf, sizeof(scrollBuf), "%d/%d", scrollOffset + 1, maxScroll + 1);
    renderer.drawText(SMALL_FONT_ID, pageWidth - 60, contentTop, scrollBuf);
  }

  GUI.drawButtonHints(renderer, "Back", "", "Up", "Down");

  renderer.displayBuffer();
}
