#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReadingStatsActivity::loadBooks() {
  books.clear();
  const auto& recentList = RECENT_BOOKS.getBooks();
  books.reserve(recentList.size());
  for (const RecentBook& rb : recentList) {
    if (!Storage.exists(rb.path.c_str())) {
      continue;
    }
    // Skip files in system/cache directories (e.g. XTCache metadata files)
    if (rb.path.find("XTCache") != std::string::npos) {
      continue;
    }
    BookStatEntry entry;
    entry.path = rb.path;
    entry.title = rb.title;
    entry.author = rb.author;
    entry.coverBmpPath = rb.coverBmpPath;
    const BookReadingStats stats = ReadingStatsStore::load(rb.path);
    entry.totalSecondsRead = stats.totalSecondsRead;
    entry.lastProgressPercent = stats.lastProgressPercent;
    books.push_back(entry);
  }
}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  loadBooks();
  selectorIndex = 0;
  viewMode = ViewMode::LIST;
  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  Activity::onExit();
  books.clear();
}

void ReadingStatsActivity::loop() {
  if (viewMode == ViewMode::DETAIL) {
    // In detail view, Back returns to list
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      viewMode = ViewMode::LIST;
      requestUpdate();
      return;
    }
    // Confirm in detail view opens the book
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!books.empty() && selectorIndex < static_cast<int>(books.size())) {
        onSelectBook(books[selectorIndex].path);
      }
      return;
    }
    return;
  }

  // LIST mode navigation
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!books.empty() && selectorIndex < static_cast<int>(books.size())) {
      viewMode = ViewMode::DETAIL;
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(books.size());
  if (listSize == 0) return;

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
    requestUpdate();
  });
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (viewMode == ViewMode::DETAIL && !books.empty() && selectorIndex < static_cast<int>(books.size())) {
    renderDetail(books[selectorIndex]);
  } else {
    renderList();
  }

  renderer.displayBuffer();
}

void ReadingStatsActivity::renderList() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  // ---- Summary row ----
  const int summaryY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  constexpr int summaryHeight = 30;

  // Compute totals across all books
  uint32_t totalSeconds = 0;
  int booksWithData = 0;
  for (const auto& entry : books) {
    totalSeconds += entry.totalSecondsRead;
    if (entry.totalSecondsRead > 0) {
      booksWithData++;
    }
  }

  char summaryBuf[64];
  if (totalSeconds > 0) {
    const std::string totalTime = ReadingStatsStore::formatTime(totalSeconds);
    snprintf(summaryBuf, sizeof(summaryBuf), "%d books  ·  %s total", static_cast<int>(books.size()),
             totalTime.c_str());
  } else {
    snprintf(summaryBuf, sizeof(summaryBuf), "%d books", static_cast<int>(books.size()));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, summaryY, summaryBuf);

  // ---- Book list ----
  const int listTop = summaryY + summaryHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (books.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    constexpr int rowHeight = 64;
    const int itemsOnScreen = listHeight / rowHeight;
    const int scrollOffset = (selectorIndex >= itemsOnScreen) ? (selectorIndex - itemsOnScreen + 1) : 0;

    for (int i = 0; i < itemsOnScreen; ++i) {
      const int bookIdx = scrollOffset + i;
      if (bookIdx >= static_cast<int>(books.size())) break;

      const BookStatEntry& entry = books[bookIdx];
      const int rowY = listTop + i * rowHeight;
      const bool selected = (bookIdx == selectorIndex);

      if (selected) {
        renderer.fillRect(metrics.contentSidePadding, rowY, pageWidth - metrics.contentSidePadding * 2, rowHeight - 2);
      } else {
        renderer.drawRect(metrics.contentSidePadding, rowY, pageWidth - metrics.contentSidePadding * 2, rowHeight - 2);
      }

      // Title (truncated to fit)
      const int textX = metrics.contentSidePadding + 8;
      const int maxTextWidth = pageWidth - metrics.contentSidePadding * 2 - 16;
      const std::string truncTitle = renderer.truncatedText(UI_12_FONT_ID, entry.title.c_str(), maxTextWidth);
      renderer.drawText(UI_12_FONT_ID, textX, rowY + 4, truncTitle.c_str(), !selected, EpdFontFamily::BOLD);

      // Stats line below title
      std::string statsLine;
      if (entry.totalSecondsRead > 0) {
        const std::string timeRead = ReadingStatsStore::formatTime(entry.totalSecondsRead);
        const uint32_t secsLeft = ReadingStatsStore::estimateSecondsLeft(entry.totalSecondsRead,
                                                                         entry.lastProgressPercent);
        statsLine = std::to_string(static_cast<int>(entry.lastProgressPercent)) + "%  ·  " + timeRead + " " +
                    tr(STR_STATS_READ_SUFFIX);
        if (secsLeft >= 60) {
          statsLine += "  ·  " + ReadingStatsStore::formatTime(secsLeft) + " " + tr(STR_STATS_LEFT_SUFFIX);
        }
      } else {
        statsLine = std::to_string(static_cast<int>(entry.lastProgressPercent)) + "%  ·  " + tr(STR_STATS_NO_DATA);
      }
      const std::string truncStats = renderer.truncatedText(UI_10_FONT_ID, statsLine.c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, textX, rowY + 4 + renderer.getLineHeight(UI_12_FONT_ID) + 2,
                        truncStats.c_str(), !selected);
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_HOME), books.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ReadingStatsActivity::renderDetail(const BookStatEntry& entry) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int textX = metrics.contentSidePadding;
  const int maxWidth = pageWidth - metrics.contentSidePadding * 2;

  // Book title (wrapped, up to 3 lines)
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, entry.title.c_str(), maxWidth, 3, EpdFontFamily::BOLD);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }

  // Author
  if (!entry.author.empty()) {
    const std::string truncAuthor = renderer.truncatedText(UI_10_FONT_ID, entry.author.c_str(), maxWidth);
    renderer.drawText(UI_10_FONT_ID, textX, y, truncAuthor.c_str(), true);
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  } else {
    y += metrics.verticalSpacing;
  }

  // Divider
  renderer.fillRect(textX, y, maxWidth, 1);
  y += 1 + metrics.verticalSpacing;

  // Progress bar
  constexpr int barHeight = 10;
  renderer.drawRect(textX, y, maxWidth, barHeight);
  const int fillWidth = maxWidth * static_cast<int>(entry.lastProgressPercent) / 100;
  if (fillWidth > 0) {
    renderer.fillRect(textX + 1, y + 1, fillWidth - 2, barHeight - 2);
  }
  y += barHeight + metrics.verticalSpacing;

  // Progress percentage
  char buf[64];
  snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(entry.lastProgressPercent));
  renderer.drawText(UI_10_FONT_ID, textX, y, buf, true);
  y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;

  // Time read
  if (entry.totalSecondsRead > 0) {
    const std::string timeRead = ReadingStatsStore::formatTime(entry.totalSecondsRead);
    snprintf(buf, sizeof(buf), "%s %s", timeRead.c_str(), tr(STR_STATS_READ_SUFFIX));
    renderer.drawText(UI_10_FONT_ID, textX, y, buf, true);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 4;

    // Estimated time left
    const uint32_t secsLeft =
        ReadingStatsStore::estimateSecondsLeft(entry.totalSecondsRead, entry.lastProgressPercent);
    if (secsLeft >= 60) {
      const std::string timeLeft = ReadingStatsStore::formatTime(secsLeft);
      snprintf(buf, sizeof(buf), "%s %s", timeLeft.c_str(), tr(STR_STATS_LEFT_SUFFIX));
      renderer.drawText(UI_10_FONT_ID, textX, y, buf, true);
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, textX, y, tr(STR_STATS_NO_DATA), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
