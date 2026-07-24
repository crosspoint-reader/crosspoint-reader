#include "ReadingCalendarActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr std::array<StrId, READING_DAY_OF_WEEK_COUNT> DAY_LABELS = {
    StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
    StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};

void drawCentered(const GfxRenderer& renderer, const int fontId, const int x, const int width, const int y,
                  const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int textWidth = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, x + std::max(0, (width - textWidth) / 2), y, text, true, style);
}
}  // namespace

void ReadingCalendarActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingCalendarActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  navigator_.onPreviousPress([this] {
    if (model_.movePrevious()) requestUpdate();
  });
  navigator_.onNextPress([this] {
    if (model_.moveNext()) requestUpdate();
  });
}

void ReadingCalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = safe.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  GUI.drawHeader(renderer, Rect{safe.x, headerTop, safe.width, metrics.headerHeight}, tr(STR_STATS_CALENDAR));

  char month[16] = "--/----";
  char dayCount[40] = "";
  if (model_.visibleMonth().isValid()) {
    snprintf(month, sizeof(month), "%02u/%04u", static_cast<unsigned>(model_.visibleMonth().month),
             static_cast<unsigned>(model_.visibleMonth().year));
  }
  if (model_.isAvailable()) {
    snprintf(dayCount, sizeof(dayCount), "%u %s", static_cast<unsigned>(model_.snapshot().readingDays),
             tr(STR_STATS_CALENDAR_DAYS_SHORT));
  }
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, safe.width, metrics.tabBarHeight}, month,
                    dayCount[0] ? dayCount : nullptr);

  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;
  if (!model_.isAvailable()) {
    const char* message = model_.snapshot().clockValid ? tr(STR_STATS_DATED_DATA_UNAVAILABLE)
                                                       : tr(STR_STATS_CALENDAR_CLOCK_UNAVAILABLE);
    const std::string displayed = renderer.truncatedText(UI_10_FONT_ID, message, safe.width - 32);
    drawCentered(renderer, UI_10_FONT_ID, safe.x + 16, safe.width - 32,
                 contentTop + std::max(0, (contentBottom - contentTop - renderer.getLineHeight(UI_10_FONT_ID)) / 2),
                 displayed.c_str());
  } else {
    const int side = metrics.contentSidePadding;
    const int gridX = safe.x + side;
    const int gridWidth = std::max(7, safe.width - side * 2);
    const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
    const int streakHeight = smallLine + 8;
    const int weekdayHeight = smallLine + 8;
    const int legendHeight = smallLine + 10;
    const int gridTop = contentTop + streakHeight + weekdayHeight;
    const int gridHeight = std::max(6, contentBottom - gridTop - legendHeight);
    const int cellWidth = gridWidth / 7;
    const int cellHeight = std::max(1, gridHeight / 6);

    char streak[64];
    snprintf(streak, sizeof(streak), "%s: %u", tr(STR_STATS_STREAK),
             static_cast<unsigned>(model_.snapshot().currentStreak));
    const std::string displayedStreak = renderer.truncatedText(SMALL_FONT_ID, streak, gridWidth);
    drawCentered(renderer, SMALL_FONT_ID, gridX, gridWidth, contentTop + 2, displayedStreak.c_str());

    for (size_t column = 0; column < DAY_LABELS.size(); ++column) {
      const int x = gridX + static_cast<int>(column) * cellWidth;
      const int width = column == DAY_LABELS.size() - 1 ? gridX + gridWidth - x : cellWidth;
      drawCentered(renderer, SMALL_FONT_ID, x, width, contentTop + streakHeight + 2, I18N.get(DAY_LABELS[column]),
                   EpdFontFamily::BOLD);
    }

    for (size_t index = 0; index < 42; ++index) {
      const ReadingCalendarCell cell = model_.cellAt(index);
      if (!cell.inMonth) continue;
      const int column = static_cast<int>(index % 7);
      const int row = static_cast<int>(index / 7);
      const int x = gridX + column * cellWidth;
      const int width = column == 6 ? gridX + gridWidth - x : cellWidth;
      const int y = gridTop + row * cellHeight;
      const int height = row == 5 ? gridTop + gridHeight - y : cellHeight;
      const int boxWidth = std::max(18, std::min(38, width - 8));
      const int boxHeight = std::max(22, std::min(42, height - 4));
      const int boxX = x + (width - boxWidth) / 2;
      const int boxY = y + (height - boxHeight) / 2;
      if (cell.today) renderer.drawRect(boxX, boxY, boxWidth, boxHeight, 2, true);

      char day[4];
      snprintf(day, sizeof(day), "%u", static_cast<unsigned>(cell.date.day));
      drawCentered(renderer, SMALL_FONT_ID, x, width, boxY + 3, day, cell.today ? EpdFontFamily::BOLD
                                                                                : EpdFontFamily::REGULAR);
      const int markerY = std::min(y + height - 7, boxY + smallLine + 6);
      const int markerX = x + width / 2;
      if (cell.read) {
        renderer.fillRect(markerX - 3, markerY, 6, 6);
      } else if (!cell.tracked && compareReadingStatsDate(cell.date, model_.snapshot().today) <= 0) {
        renderer.drawLine(markerX - 4, markerY + 3, markerX + 4, markerY + 3);
      }
    }

    const int legendY = gridTop + gridHeight + 4;
    const int half = gridWidth / 2;
    renderer.fillRect(gridX + 4, legendY + (smallLine - 5) / 2, 5, 5);
    const std::string readLabel = renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_CALENDAR_READ_DAY), half - 20);
    renderer.drawText(SMALL_FONT_ID, gridX + 14, legendY, readLabel.c_str());
    renderer.drawLine(gridX + half + 3, legendY + smallLine / 2, gridX + half + 11, legendY + smallLine / 2);
    const std::string missingLabel =
        renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_CALENDAR_NO_HISTORY), gridWidth - half - 20);
    renderer.drawText(SMALL_FONT_ID, gridX + half + 16, legendY, missingLabel.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", model_.canMovePrevious() ? tr(STR_STATS_PREVIOUS) : "",
                                            model_.canMoveNext() ? tr(STR_STATS_NEXT) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
