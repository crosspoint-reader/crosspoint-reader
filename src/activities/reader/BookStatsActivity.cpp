#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"


BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const BookReadingStats& stats)
    : Activity("BookStats", renderer, mappedInput), bookTitle(title), stats(stats) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int screenWidth = renderer.getScreenWidth();
  constexpr int marginX = 20;
  constexpr int lineHeight = 30;
  constexpr int sectionGap = 10;
  int y = 20;

  // --- Title ---
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_READING_STATS), true, EpdFontFamily::BOLD);
  y += lineHeight + sectionGap;

  // --- Book title (truncated) ---
  const int maxWidth = screenWidth - marginX * 2;
  const std::string truncTitle =
      renderer.truncatedText(UI_10_FONT_ID, bookTitle.c_str(), maxWidth, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(UI_10_FONT_ID, y, truncTitle.c_str(), true, EpdFontFamily::REGULAR);
  y += lineHeight + sectionGap;

  // Divider
  renderer.drawLine(marginX, y, screenWidth - marginX, y, true);
  y += sectionGap + 5;

  // --- Stat rows ---
  char valueBuf[32];

  // Sessions
  snprintf(valueBuf, sizeof(valueBuf), "%u", static_cast<unsigned>(stats.sessionCount));
  {
    const std::string line = std::string(tr(STR_STATS_SESSIONS)) + valueBuf;
    renderer.drawText(UI_10_FONT_ID, marginX, y, line.c_str(), true);
  }
  y += lineHeight;

  // Total reading time
  BookReadingStats::formatDuration(stats.totalReadingSeconds, valueBuf, sizeof(valueBuf));
  {
    const std::string line = std::string(tr(STR_STATS_TOTAL_TIME)) + valueBuf;
    renderer.drawText(UI_10_FONT_ID, marginX, y, line.c_str(), true);
  }
  y += lineHeight;

  // Pages turned
  snprintf(valueBuf, sizeof(valueBuf), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
  {
    const std::string line = std::string(tr(STR_STATS_PAGES_TURNED)) + valueBuf;
    renderer.drawText(UI_10_FONT_ID, marginX, y, line.c_str(), true);
  }
  y += lineHeight;

  // Average session time
  if (stats.sessionCount > 0) {
    BookReadingStats::formatDuration(stats.totalReadingSeconds / stats.sessionCount, valueBuf, sizeof(valueBuf));
  } else {
    BookReadingStats::formatDuration(0, valueBuf, sizeof(valueBuf));
  }
  {
    const std::string line = std::string(tr(STR_STATS_AVG_SESSION)) + valueBuf;
    renderer.drawText(UI_10_FONT_ID, marginX, y, line.c_str(), true);
  }

  // Button hint
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
