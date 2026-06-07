#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <StatsFormat.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "ReadingStatsDetailActivity.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Vertical space reserved above the list for the totals block (two text rows).
// Sized so the list's selection highlight (drawn at contentTop - 2) clears the
// second totals line; too small and selecting the first row overlaps the totals.
constexpr int TOTALS_BLOCK_HEIGHT = 72;
}  // namespace

void ReadingStatsActivity::loadStats() {
  books = READING_STATS.books();  // copy snapshot
  std::sort(books.begin(), books.end(), [](const reading_stats::BookStats& a, const reading_stats::BookStats& b) {
    return a.totalReadingMs > b.totalReadingMs;
  });
  totalPages = READING_STATS.totalPagesRead();
  totalMs = READING_STATS.totalReadingMs();
}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  loadStats();
  selectorIndex = 0;
  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  Activity::onExit();
  books.clear();
}

void ReadingStatsActivity::loop() {
  const int pageItems =
      UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true, TOTALS_BLOCK_HEIGHT);

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!books.empty() && selectorIndex < books.size()) {
      startActivityForResult(std::make_unique<ReadingStatsDetailActivity>(renderer, mappedInput, books[selectorIndex],
                                                                          totalPages, totalMs),
                             [](const ActivityResult&) {});
    }
    return;
  }

  const int listSize = static_cast<int>(books.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS_TITLE));

  const int headerBottom = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const std::string line1 =
      std::string(tr(STR_READING_STATS_TOTAL)) + ": " + std::to_string(totalPages) + " " + tr(STR_READING_STATS_PAGES);
  const std::string line2 = reading_stats::formatDurationMs(totalMs) + "  -  " + std::to_string(books.size()) + " " +
                            tr(STR_READING_STATS_BOOKS);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, headerBottom + 14, line1.c_str());
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, headerBottom + 36, line2.c_str());

  const int contentTop = headerBottom + TOTALS_BLOCK_HEIGHT;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (books.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_READING_STATS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(books.size()),
        static_cast<int>(selectorIndex),
        [this](int index) { return reading_stats::pathToDisplayName(books[index].bookPath); },
        [this](int index) {
          return std::to_string(books[index].pagesRead) + " " + tr(STR_READING_STATS_PAGES) + " - " +
                 reading_stats::formatDurationMs(books[index].totalReadingMs);
        },
        [](int) -> UIIcon { return Book; },
        [this](int index) {
          const auto& b = books[index];
          const uint32_t pph =
              b.totalReadingMs
                  ? static_cast<uint32_t>(static_cast<uint64_t>(b.pagesRead) * 3600000ULL / b.totalReadingMs)
                  : 0u;
          return std::to_string(pph) + tr(STR_READING_STATS_PER_HOUR);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
