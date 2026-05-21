#include "ReadingStatsActivity.h"

#include <Arduino.h>  // millis()
#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include "MappedInputManager.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"
#include "ReadingStatsBookListActivity.h"
#include "components/CardLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// "1h 23m" / "23m 45s" / "12s" — compact so a long row fits on the X3.
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

// Pages per minute, rounded to 1 decimal. Returns "—" when there's not enough
// data (fewer than a minute total) so we don't display "120.0 ppm" when only
// a handful of seconds have been recorded.
std::string formatPagesPerMin(uint32_t pages, uint32_t seconds) {
  if (seconds < 60 || pages == 0) {
    return tr(STR_READING_STATS_UNKNOWN);
  }
  const float ppm = (pages * 60.0f) / seconds;
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", ppm);
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
  // Confirm opens the all-books list when there's anything to drill into.
  // Suppressed when the store is empty so the button hint never lies.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !READING_STATS.getBooks().empty()) {
    startActivityForResult(std::make_unique<ReadingStatsBookListActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  // If a reading session happens to be live, tick at most once per second so
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

  const int startY = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  CardLayout::Config cfg;
  cfg.outerMarginX = metrics.verticalSpacing * 2;
  CardLayout layout(renderer, contentRect, startY, cfg);

  const auto& store = READING_STATS;
  auto& tracker = globalReadingSessionTracker();

  // ---- Live session card ----
  if (tracker.isActive()) {
    layout.card(tr(STR_READING_STATS_CURRENT_SESSION), [&](CardLayout::Body& b) {
      b.rowLR(tr(STR_READING_STATS_TOTAL_TIME), formatDuration(tracker.getLiveSeconds()));
      b.rowLR(tr(STR_READING_STATS_PAGES), std::to_string(tracker.getLivePages()));
    });
  }

  // ---- All-time card ----
  layout.card(tr(STR_READING_STATS_TOTAL_TIME), [&](CardLayout::Body& b) {
    if (store.getGlobalTotalSeconds() == 0) {
      b.centeredMessage(tr(STR_READING_STATS_NO_DATA));
      return;
    }

    // 4-cell stat grid: sessions / books / current streak / longest streak.
    // Streaks read "—" when the clock has never been wall-anchored.
    const uint16_t today = currentLocalDayIndex();
    const bool haveStreak = today != 0 && !store.getGlobalDays().empty();
    const std::string curStreak =
        haveStreak ? std::to_string(store.computeCurrentStreak(today)) : std::string(tr(STR_READING_STATS_UNKNOWN));
    const std::string maxStreak =
        haveStreak ? std::to_string(store.computeLongestStreak()) : std::string(tr(STR_READING_STATS_UNKNOWN));
    b.statGrid({{{std::to_string(store.getGlobalTotalSessions()), tr(STR_READING_STATS_SESSIONS)},
                 {std::to_string(store.getBookCount()), tr(STR_READING_STATS_BOOKS)},
                 {curStreak, tr(STR_READING_STATS_STREAK)},
                 {maxStreak, tr(STR_READING_STATS_LONGEST)}}});

    b.rowLR(tr(STR_READING_STATS_TOTAL_TIME), formatDuration(store.getGlobalTotalSeconds()));
    b.rowLR(tr(STR_READING_STATS_PAGES), std::to_string(store.getGlobalTotalPagesTurned()));
    b.rowLR(tr(STR_READING_STATS_PAGES_PER_MIN),
            formatPagesPerMin(store.getGlobalTotalPagesTurned(), store.getGlobalTotalSeconds()));
    // Only show the finished row once the user has actually finished a book —
    // otherwise it's just clutter saying "0".
    if (store.getFinishedBookCount() > 0) {
      b.rowLR(tr(STR_READING_STATS_FINISHED), std::to_string(store.getFinishedBookCount()));
    }
  });

  // ---- 30-day sparkline card ----
  const uint16_t today = currentLocalDayIndex();
  if (today != 0 && !store.getGlobalDays().empty()) {
    layout.card(tr(STR_READING_STATS_LAST_30D), [&](CardLayout::Body& b) {
      constexpr int kSparkDays = 30;
      constexpr int kSparkHeight = 32;
      constexpr int kBarGap = 1;
      const int innerWidth = b.innerWidth();
      const int innerLeft = b.innerLeft();
      const int barWidth = std::max(2, (innerWidth - (kSparkDays - 1) * kBarGap) / kSparkDays);
      const int totalSpan = barWidth * kSparkDays + kBarGap * (kSparkDays - 1);
      const int sparkOriginX = innerLeft + (innerWidth - totalSpan) / 2;
      const int sparkOriginY = b.currentY();

      uint32_t maxSeconds = 1;
      for (int i = 0; i < kSparkDays; ++i) {
        const uint16_t d = (today > static_cast<uint16_t>(kSparkDays - 1 - i))
                               ? static_cast<uint16_t>(today - (kSparkDays - 1 - i))
                               : 0;
        const uint32_t s = store.getSecondsForDay(d);
        if (s > maxSeconds) maxSeconds = s;
      }

      renderer.drawLine(sparkOriginX, sparkOriginY + kSparkHeight, sparkOriginX + totalSpan,
                        sparkOriginY + kSparkHeight, true);
      for (int i = 0; i < kSparkDays; ++i) {
        const uint16_t d = (today > static_cast<uint16_t>(kSparkDays - 1 - i))
                               ? static_cast<uint16_t>(today - (kSparkDays - 1 - i))
                               : 0;
        const uint32_t s = store.getSecondsForDay(d);
        const int barX = sparkOriginX + i * (barWidth + kBarGap);
        const int h = s == 0 ? 1 : std::max<int>(2, (s * kSparkHeight) / maxSeconds);
        renderer.fillRect(barX, sparkOriginY + kSparkHeight - h, barWidth, h, true);
      }
      b.advance(kSparkHeight + 2);
    });
  }

  // ---- Top books card ----
  if (!store.getBooks().empty()) {
    std::vector<const BookReadingStats*> sorted;
    sorted.reserve(store.getBooks().size());
    std::transform(store.getBooks().begin(), store.getBooks().end(), std::back_inserter(sorted),
                   [](const BookReadingStats& b) { return &b; });
    std::sort(sorted.begin(), sorted.end(),
              [](const BookReadingStats* a, const BookReadingStats* b) { return a->totalSeconds > b->totalSeconds; });

    layout.card(tr(STR_READING_STATS_TOP_BOOKS), [&](CardLayout::Body& b) {
      const int ellipsisWidth = renderer.getTextWidth(UI_10_FONT_ID, "…");
      constexpr int kTitleGap = 8;
      const size_t shown = std::min<size_t>(sorted.size(), 3);
      const int innerLeft = b.innerLeft();
      const int innerRight = b.innerRight();
      for (size_t i = 0; i < shown; ++i) {
        const auto* bk = sorted[i];
        const std::string time = formatDuration(bk->totalSeconds);
        const int timeWidth = renderer.getTextWidth(UI_10_FONT_ID, time.c_str());
        renderer.drawText(UI_10_FONT_ID, innerRight - timeWidth, b.currentY(), time.c_str());

        std::string label = bk->title.empty() ? bk->docId : bk->title;
        const int maxLabelWidth = (innerRight - timeWidth - kTitleGap) - innerLeft;
        if (maxLabelWidth > 0 && renderer.getTextWidth(UI_10_FONT_ID, label.c_str()) > maxLabelWidth) {
          while (!label.empty() &&
                 renderer.getTextWidth(UI_10_FONT_ID, label.c_str()) + ellipsisWidth > maxLabelWidth) {
            utf8RemoveLastChar(label);
          }
          label += "…";
        }
        renderer.drawText(UI_10_FONT_ID, innerLeft, b.currentY(), label.c_str(), true, EpdFontFamily::BOLD);
        b.advance(b.rowStep());
      }
    });
  }

  const char* btn2 = store.getBooks().empty() ? "" : tr(STR_READING_STATS_BOOK_LIST);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
