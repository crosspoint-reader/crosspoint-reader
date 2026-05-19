#include "ReadingStatsActivity.h"

#include <Arduino.h>  // millis()
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "MappedInputManager.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// "1h 23m" / "23m 45s" / "12s" — Phase 1 keeps it compact so a long row fits
// the right column without truncation on the X3's narrow screen.
std::string formatDuration(uint32_t totalSeconds) {
  const uint32_t h = totalSeconds / 3600;
  const uint32_t m = (totalSeconds % 3600) / 60;
  const uint32_t s = totalSeconds % 60;
  char buf[24];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%uh %02um", h, m);
  } else if (m > 0) {
    snprintf(buf, sizeof(buf), "%um %02us", m, s);
  } else {
    snprintf(buf, sizeof(buf), "%us", s);
  }
  return buf;
}

}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // If a reading session happens to be live (e.g. a future entry point lets
  // the user pop this screen mid-read), tick at most once per second so
  // "this session" moves visibly without hammering the e-ink panel.
  if (globalReadingSessionTracker().isActive()) {
    const uint32_t now = millis();
    if (now - lastLiveRefreshMs >= 1000) {
      lastLiveRefreshMs = now;
      requestUpdate();
    }
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  renderer.clearScreen();

  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_READING_STATS), nullptr);

  const int leftX = contentRect.x + metrics.verticalSpacing * 3;
  const int valueX = contentRect.x + contentRect.width / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowStep = lineH + 2;
  const int subHeaderHeight = lineH + 6;
  int y = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto drawSection = [&](const char* title) {
    GUI.drawSubHeader(renderer, Rect{contentRect.x, y, contentRect.width, subHeaderHeight}, title);
    y += subHeaderHeight + 2;
  };
  auto drawRow = [&](const char* label, const std::string& value) {
    renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, valueX, y, value.c_str());
    y += rowStep;
  };

  const auto& store = READING_STATS;
  auto& tracker = globalReadingSessionTracker();

  // ---- Live session (only if currently reading) ----
  if (tracker.isActive()) {
    drawSection(tr(STR_READING_STATS_CURRENT_SESSION));
    drawRow(tr(STR_READING_STATS_TOTAL_TIME), formatDuration(tracker.getLiveSeconds()));
    drawRow(tr(STR_READING_STATS_PAGES), std::to_string(tracker.getLivePages()));
  }

  // ---- All time ----
  drawSection(tr(STR_READING_STATS_TOTAL_TIME));
  if (store.getGlobalTotalSeconds() == 0) {
    drawRow("", tr(STR_READING_STATS_NO_DATA));
  } else {
    drawRow(tr(STR_READING_STATS_TOTAL_TIME), formatDuration(store.getGlobalTotalSeconds()));
    drawRow(tr(STR_READING_STATS_SESSIONS), std::to_string(store.getGlobalTotalSessions()));
    drawRow(tr(STR_READING_STATS_PAGES), std::to_string(store.getGlobalTotalPagesTurned()));
    drawRow(tr(STR_READING_STATS_BOOKS), std::to_string(store.getBookCount()));
  }

  // ---- Top books (up to 3 by total time) ----
  if (!store.getBooks().empty()) {
    std::vector<const BookReadingStats*> sorted;
    sorted.reserve(store.getBooks().size());
    for (const auto& b : store.getBooks()) sorted.push_back(&b);
    std::sort(sorted.begin(), sorted.end(),
              [](const BookReadingStats* a, const BookReadingStats* b) { return a->totalSeconds > b->totalSeconds; });

    drawSection(tr(STR_READING_STATS_TOP_BOOKS));
    const size_t shown = std::min<size_t>(sorted.size(), 3);
    for (size_t i = 0; i < shown; ++i) {
      const auto* b = sorted[i];
      // Use the title when known; fall back to docId so the row is never
      // empty even before metadata is recorded.
      std::string label = b->title.empty() ? b->docId : b->title;
      // Trim to fit; 22 chars keeps it in the left column at UI_10.
      if (label.size() > 22) {
        label.resize(22);
        label += "…";
      }
      drawRow(label.c_str(), formatDuration(b->totalSeconds));
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
