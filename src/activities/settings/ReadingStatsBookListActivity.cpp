#include "ReadingStatsBookListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "MappedInputManager.h"
#include "ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Same compact format as the main stats screen — kept inline rather than
// shared via a header to keep this slice's footprint small. If a third
// caller appears we'll lift it into a util.
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

void ReadingStatsBookListActivity::rebuildSortedBooks() {
  sortedBooks.clear();
  sortedBooks.reserve(READING_STATS.getBooks().size());
  std::transform(READING_STATS.getBooks().begin(), READING_STATS.getBooks().end(), std::back_inserter(sortedBooks),
                 [](const BookReadingStats& b) { return &b; });
  std::sort(sortedBooks.begin(), sortedBooks.end(),
            [](const BookReadingStats* a, const BookReadingStats* b) { return a->totalSeconds > b->totalSeconds; });
}

void ReadingStatsBookListActivity::onEnter() {
  Activity::onEnter();
  rebuildSortedBooks();
  if (selectedIndex >= static_cast<int>(sortedBooks.size())) {
    selectedIndex = 0;
  }
  requestUpdate();
}

void ReadingStatsBookListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!sortedBooks.empty()) {
    buttonNavigator.onNextList(selectedIndex, static_cast<int>(sortedBooks.size()), [this]() { requestUpdate(); });
    buttonNavigator.onPreviousList(selectedIndex, static_cast<int>(sortedBooks.size()), [this]() { requestUpdate(); });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const BookReadingStats* book = sortedBooks[selectedIndex];
      startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(renderer, mappedInput, book->docId),
                             // After detail closes the underlying store hasn't changed (it's
                             // read-only), so just redraw — selectedIndex is preserved.
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  }
}

void ReadingStatsBookListActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  renderer.clearScreen();

  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_READING_STATS_BOOK_LIST), nullptr);

  const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      contentRect.height - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2);

  if (sortedBooks.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_READING_STATS_NO_DATA));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight},
        static_cast<int>(sortedBooks.size()), selectedIndex,
        [this](int index) {
          const auto* b = sortedBooks[index];
          // Title is the primary label; fall back to docId so a row without
          // metadata is still recognizable. Finished books get a leading
          // checkmark so the user can spot completions at a glance.
          std::string label = b->title.empty() ? b->docId : b->title;
          if (b->finishedCount > 0) label = "✓ " + label;
          return label;
        },
        [this](int index) {
          // Subtitle row: author, when known. Empty string is treated by the
          // theme as "no subtitle" and the row collapses to a single line.
          return sortedBooks[index]->author;
        },
        nullptr, [this](int index) { return formatDuration(sortedBooks[index]->totalSeconds); }, true);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), sortedBooks.empty() ? "" : tr(STR_SELECT),
                            sortedBooks.empty() ? "" : tr(STR_DIR_UP), sortedBooks.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
