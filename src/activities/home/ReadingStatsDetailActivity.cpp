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

  // --- Stat lines: spacing driven by the real line height, not a magic constant. ---
  const int statLh = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineStep = statLh + 6;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 12;

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

  // --- Comparative speed bars: this book vs. all-books average. ---
  // Each row is a single line "<label>  [====bar====]  <value>/h" with the label
  // in a left column, the value right-aligned, and the bar centered vertically on
  // the same baseline so nothing overlaps and the value lines up with the bar.
  y += statLh;  // gap before the bar block
  const uint32_t maxSpeed = std::max({speed, avgSpeed, uint32_t{1}});

  const char* thisLabel = tr(STR_READING_STATS_THIS_BOOK);
  const char* avgLabel = tr(STR_READING_STATS_AVERAGE);
  const std::string thisVal = std::to_string(speed) + tr(STR_READING_STATS_PER_HOUR);
  const std::string avgVal = std::to_string(avgSpeed) + tr(STR_READING_STATS_PER_HOUR);

  const int smLh = renderer.getLineHeight(SMALL_FONT_ID);
  const int barH = 12;
  const int rowH = std::max(smLh, barH);
  const int gap = 10;

  const int labelColW =
      std::max(renderer.getTextWidth(SMALL_FONT_ID, thisLabel), renderer.getTextWidth(SMALL_FONT_ID, avgLabel)) + gap;
  const int valueColW = std::max(renderer.getTextWidth(SMALL_FONT_ID, thisVal.c_str()),
                                 renderer.getTextWidth(SMALL_FONT_ID, avgVal.c_str())) +
                        gap;
  const int barX = x + labelColW;
  const int barMaxW = std::max(1, pageWidth - barX - valueColW - x);

  auto bar = [&](const char* label, uint32_t value, const std::string& valueStr) {
    const int textTop = y + (rowH - smLh) / 2;  // vertically center the text in the row
    const int barY = y + (rowH - barH) / 2;     // bar shares the same vertical centerline
    renderer.drawText(SMALL_FONT_ID, x, textTop, label);
    const int w = static_cast<int>(static_cast<int64_t>(barMaxW) * value / maxSpeed);
    renderer.drawRect(barX, barY, barMaxW, barH);
    renderer.fillRect(barX, barY, std::max(w, 1), barH);
    const int valW = renderer.getTextWidth(SMALL_FONT_ID, valueStr.c_str());
    renderer.drawText(SMALL_FONT_ID, pageWidth - x - valW, textTop, valueStr.c_str());
    y += rowH + gap;
  };

  bar(thisLabel, speed, thisVal);
  bar(avgLabel, avgSpeed, avgVal);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
