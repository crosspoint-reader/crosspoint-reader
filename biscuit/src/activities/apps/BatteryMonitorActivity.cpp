#include "BatteryMonitorActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BatteryMonitorActivity::onEnter() {
  Activity::onEnter();
  historyHead = 0;
  historyCount = 0;
  for (int i = 0; i < HISTORY_SIZE; i++) history[i].percent = 0;
  takeSample();
  lastSample = millis();
  requestUpdate();
}

void BatteryMonitorActivity::onExit() { Activity::onExit(); }

void BatteryMonitorActivity::takeSample() {
  currentPercent = powerManager.getBatteryPercentage();
  history[historyHead].percent = currentPercent;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

void BatteryMonitorActivity::loop() {
  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    takeSample();
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    takeSample();
    lastSample = now;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void BatteryMonitorActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Battery Monitor");

  // Large centered percentage
  char buf[16];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)currentPercent);
  int midY = metrics.topPadding + metrics.headerHeight + (pageHeight - metrics.topPadding - metrics.headerHeight
             - metrics.buttonHintsHeight - 120) / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, midY, buf, true, EpdFontFamily::BOLD);

  // Sub-label
  const char* label;
  if (currentPercent >= 80) label = "Excellent";
  else if (currentPercent >= 50) label = "Good";
  else if (currentPercent >= 20) label = "Low";
  else label = "Critical";
  renderer.drawCenteredText(UI_10_FONT_ID, midY + 50, label);

  // Graph area at bottom — show history as a simple line graph
  if (historyCount >= 2) {
    const int graphLeft = metrics.contentSidePadding;
    const int graphRight = pageWidth - metrics.contentSidePadding;
    const int graphBottom = pageHeight - metrics.buttonHintsHeight - 20;
    const int graphTop = graphBottom - 100;
    const int graphW = graphRight - graphLeft;
    const int graphH = graphBottom - graphTop;

    // Draw graph border
    renderer.drawRect(graphLeft, graphTop, graphW, graphH, true);

    // Plot points — walk history oldest to newest
    // The ring buffer's oldest entry is at index (historyHead - historyCount + HISTORY_SIZE) % HISTORY_SIZE
    int startIdx = (historyHead - historyCount + HISTORY_SIZE) % HISTORY_SIZE;
    int prevX = -1, prevY = -1;
    for (int i = 0; i < historyCount; i++) {
      int idx = (startIdx + i) % HISTORY_SIZE;
      int pct = history[idx].percent;
      if (pct > 100) pct = 100;
      int px = graphLeft + 1 + (i * (graphW - 2)) / (historyCount - 1);
      int py = graphBottom - 1 - (pct * (graphH - 2)) / 100;
      if (prevX >= 0) {
        // Draw line segment between prev and current point
        int dx = px - prevX;
        int dy = py - prevY;
        int absDx = dx < 0 ? -dx : dx;
        int absDy = dy < 0 ? -dy : dy;
        int steps = (absDx > absDy ? absDx : absDy);
        if (steps < 1) steps = 1;
        for (int s = 0; s <= steps; s++) {
          int lx = prevX + (dx * s) / steps;
          int ly = prevY + (dy * s) / steps;
          renderer.drawPixel(lx, ly, true);
          // Slightly thicker
          renderer.drawPixel(lx, ly + 1, true);
        }
      }
      prevX = px;
      prevY = py;
    }

    // Graph label
    renderer.drawText(SMALL_FONT_ID, graphLeft, graphTop - 16, "Battery history (30s intervals)");
  }

  const char* confirmLabel = "Refresh";
  GUI.drawButtonHints(renderer, "Back", confirmLabel, "", "");

  renderer.displayBuffer();
}
