#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
std::string formatDuration(const uint32_t seconds) {
  char value[24];
  const uint32_t minutes = seconds / 60;
  if (minutes == 0) {
    snprintf(value, sizeof(value), "<1m");
  } else if (minutes < 60) {
    snprintf(value, sizeof(value), "%lum", static_cast<unsigned long>(minutes));
  } else {
    const uint32_t hours = minutes / 60;
    const uint32_t remainder = minutes % 60;
    if (remainder == 0) {
      snprintf(value, sizeof(value), "%luh", static_cast<unsigned long>(hours));
    } else {
      snprintf(value, sizeof(value), "%luh %lum", static_cast<unsigned long>(hours),
               static_cast<unsigned long>(remainder));
    }
  }
  return value;
}

std::string formatPace(const uint16_t seconds) {
  if (seconds == 0) return "-";
  if (seconds < 60) return std::to_string(seconds) + "s";
  return formatDuration(seconds);
}
}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string bookTitle, const BookReadingStats& bookStats,
                                           const GlobalReadingStats& globalStats)
    : Activity("ReadingStats", renderer, mappedInput), bookTitle(std::move(bookTitle)) {
  ReadingStatsDateTime now;
  const uint16_t currentStreak =
      getCurrentLocalReadingStatsDateTime(now) ? globalStats.currentReadingStreak(&now.date) : 0;

  rows = {{{StrId::STR_STATS_BOOK_TIME, formatDuration(bookStats.totalReadingSeconds)},
           {StrId::STR_STATS_BOOK_SESSIONS, std::to_string(bookStats.sessionCount)},
           {StrId::STR_STATS_PAGES_TURNED, std::to_string(bookStats.totalPagesTurned)},
           {StrId::STR_STATS_AVG_PAGE, formatPace(bookStats.avgSecondsPerForwardPage)},
           {StrId::STR_STATS_TIME_LEFT,
            bookStats.estimatedTimeLeftSeconds > 0 ? formatDuration(bookStats.estimatedTimeLeftSeconds) : "-"},
           {StrId::STR_STATS_ALL_TIME, formatDuration(globalStats.totalReadingSeconds)},
           {StrId::STR_STATS_STREAK, std::to_string(currentStreak)}}};
}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(rows.size()));
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(rows.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = screen.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - metrics.buttonHintsHeight;

  GUI.drawHeader(renderer, Rect{screen.x, headerTop, screen.width, metrics.headerHeight}, tr(STR_READING_STATS));
  GUI.drawSubHeader(renderer, Rect{screen.x, subHeaderTop, screen.width, metrics.tabBarHeight}, bookTitle.c_str());
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentBottom - contentTop}, rows.size(), selectedIndex,
      [this](const int index) { return I18N.get(rows[index].label); }, nullptr, nullptr,
      [this](const int index) { return rows[index].value; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
