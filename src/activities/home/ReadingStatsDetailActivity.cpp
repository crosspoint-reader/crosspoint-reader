#include "ReadingStatsDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <StatsFormat.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReadingStatsDetailActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingStatsDetailActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ReadingStatsDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 reading_stats::pathToDisplayName(book.bookPath).c_str());

  const uint32_t speed = reading_stats::pagesPerHour(book.pagesRead, book.totalReadingMs);
  const uint32_t avgPage = reading_stats::avgMsPerPage(book.totalReadingMs, book.pagesRead);
  const uint32_t avgSession = reading_stats::avgMsPerSession(book.totalReadingMs, book.sessionCount);
  const uint32_t avgPagesSession = book.sessionCount ? book.pagesRead / book.sessionCount : 0;
  const uint32_t avgSpeed = reading_stats::pagesPerHour(globalPages, globalMs);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 18;
  const int lineStep = 26;

  auto statLine = [&](const char* label, const std::string& value) {
    const std::string s = std::string(label) + ": " + value;
    renderer.drawText(UI_10_FONT_ID, x, y, s.c_str());
    y += lineStep;
  };

  statLine(tr(STR_READING_STATS_LABEL_PAGES), std::to_string(book.pagesRead));
  statLine(tr(STR_READING_STATS_TIME), reading_stats::formatDurationMs(book.totalReadingMs));
  statLine(tr(STR_READING_STATS_SESSIONS), std::to_string(book.sessionCount));
  statLine(tr(STR_READING_STATS_SPEED), std::to_string(speed) + tr(STR_READING_STATS_PER_HOUR));
  statLine(tr(STR_READING_STATS_PER_PAGE), reading_stats::formatDurationMs(avgPage));
  statLine(tr(STR_READING_STATS_PER_SESSION), reading_stats::formatDurationMs(avgSession) + " - " +
                                                  std::to_string(avgPagesSession) + " " + tr(STR_READING_STATS_PAGES));

  // Comparative speed bars: this book vs. all-books average.
  y += 8;
  const uint32_t maxSpeed = std::max({speed, avgSpeed, uint32_t{1}});
  const int valueColW = 70;
  const int barMaxW = pageWidth - 2 * x - valueColW;
  const int barH = 14;

  auto bar = [&](const char* label, uint32_t value) {
    renderer.drawText(SMALL_FONT_ID, x, y, label);
    y += 18;
    const int w = static_cast<int>(static_cast<int64_t>(barMaxW) * value / maxSpeed);
    renderer.drawRect(x, y, barMaxW, barH);
    renderer.fillRect(x, y, std::max(w, 1), barH);
    const std::string v = std::to_string(value) + tr(STR_READING_STATS_PER_HOUR);
    renderer.drawText(SMALL_FONT_ID, x + barMaxW + 8, y + barH - 2, v.c_str());
    y += barH + 12;
  };

  bar(tr(STR_READING_STATS_THIS_BOOK), speed);
  bar(tr(STR_READING_STATS_AVERAGE), avgSpeed);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
