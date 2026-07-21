#include "DashboardTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "RecentBooksStore.h"
#include "activities/home/DashboardProgress.h"
#include "activities/home/HomeBookSummary.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/themes/dashboard/DashboardLayout.h"
#include "fontIds.h"

namespace {

constexpr int CORNER_RADIUS = 6;

void formatDuration(const uint32_t seconds, char* out, const size_t outSize) {
  const uint32_t totalMinutes = seconds / 60;
  const uint32_t hours = totalMinutes / 60;
  const uint32_t minutes = totalMinutes % 60;
  if (hours == 0) {
    snprintf(out, outSize, "%lu min", static_cast<unsigned long>(minutes));
  } else if (minutes == 0) {
    snprintf(out, outSize, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(out, outSize, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

void drawValueRow(GfxRenderer& renderer, const int x, const int y, const int width, const char* label,
                  const char* value) {
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
  const int labelWidth = std::max(0, width - valueWidth - 12);
  const std::string safeLabel = renderer.truncatedText(SMALL_FONT_ID, label, labelWidth);
  renderer.drawText(SMALL_FONT_ID, x, y, safeLabel.c_str());
  renderer.drawText(UI_10_FONT_ID, x + std::max(0, width - valueWidth), y - 2, value, true, EpdFontFamily::BOLD);
}

void drawStatus(GfxRenderer& renderer, const Rect rect, const char* text) {
  const auto lines = renderer.wrappedText(SMALL_FONT_ID, text, rect.width, 3);
  int y = rect.y;
  for (const std::string& line : lines) {
    renderer.drawText(SMALL_FONT_ID, rect.x, y, line.c_str());
    y += renderer.getLineHeight(SMALL_FONT_ID);
  }
}

}  // namespace

Rect DashboardTheme::getHomeCoverCacheRect(const Rect tileRect) const {
  return DashboardLayout::calculate(tileRect).cover;
}

void DashboardTheme::drawHomeContent(GfxRenderer& renderer, const Rect rect, const std::vector<RecentBook>& recentBooks,
                                     const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                     bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                     const HomeBookSummary& summary) const {
  const DashboardLayout layout = DashboardLayout::calculate(rect);
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const RecentBook& book = recentBooks.front();
  if (!bufferRestored || !coverRendered) {
    bool drewCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, DashboardMetrics::values.homeCoverHeight);
      HalFile file;
      if (Storage.openFileForRead("DASH", path, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height);
          drewCover = true;
        }
      }
    }
    if (!drewCover) {
      renderer.drawRect(layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height);
      renderer.fillRectDither(layout.cover.x, layout.cover.y + layout.cover.height / 3, layout.cover.width,
                              layout.cover.height * 2 / 3, Color::LightGray);
      renderer.drawIcon(CoverIcon, layout.cover.x + 16, layout.cover.y + 16, 32);
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
  renderer.drawRect(layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height);

  if (selectorIndex == 0) {
    renderer.drawRoundedRect(layout.content.x - 6, layout.content.y - 6, layout.content.width + 12,
                             layout.content.height + 12, 2, CORNER_RADIUS, true, true, true, true, true);
  }

  char duration[24]{};
  char value[32]{};
  const int statsLineHeight = 42;
  int statsY = layout.stats.y + 4;
  renderer.drawText(UI_12_FONT_ID, layout.stats.x, statsY, tr(STR_STATS_THIS_BOOK), true, EpdFontFamily::BOLD);
  statsY += 45;
  if (summary.bookStatsState == DashboardMetricState::Available) {
    formatDuration(summary.bookReadingSeconds, duration, sizeof(duration));
    drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_READING_TIME), duration);
    statsY += statsLineHeight;
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(summary.bookPagesTurned));
    drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_PAGES), value);
    statsY += statsLineHeight;
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(summary.bookSessions));
    drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_SESSIONS), value);
    statsY += statsLineHeight;
    if (summary.hasChapterPage) {
      snprintf(value, sizeof(value), "%u / %u", static_cast<unsigned>(summary.chapterPageCurrent),
               static_cast<unsigned>(summary.chapterPageTotal));
      drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_CHAPTER_PAGE), value);
    }
  } else {
    const char* status = tr(STR_STATS_UNAVAILABLE);
    if (summary.bookStatsState == DashboardMetricState::NoData) {
      status = tr(STR_DASH_NO_READING_DATA);
    } else if (summary.bookStatsState == DashboardMetricState::NotTracked) {
      status = tr(STR_DASH_NOT_TRACKED);
    }
    drawStatus(renderer, Rect{layout.stats.x, statsY, layout.stats.width, layout.stats.height - 45}, status);
    if (summary.hasChapterPage) {
      statsY += statsLineHeight * 2;
      snprintf(value, sizeof(value), "%u / %u", static_cast<unsigned>(summary.chapterPageCurrent),
               static_cast<unsigned>(summary.chapterPageTotal));
      drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_CHAPTER_PAGE), value);
    }
  }

  int titleY = layout.title.y;
  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), layout.title.width, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, layout.title.x, titleY, title.c_str(), true, EpdFontFamily::BOLD);
  titleY += renderer.getLineHeight(UI_12_FONT_ID);
  if (!book.author.empty()) {
    const std::string author = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), layout.title.width);
    renderer.drawText(SMALL_FONT_ID, layout.title.x, titleY, author.c_str());
    titleY += renderer.getLineHeight(SMALL_FONT_ID);
  }
  if (!summary.chapterTitle.empty() &&
      titleY + renderer.getLineHeight(SMALL_FONT_ID) <= layout.title.y + layout.title.height) {
    const std::string chapter = renderer.truncatedText(SMALL_FONT_ID, summary.chapterTitle.c_str(), layout.title.width);
    renderer.drawText(SMALL_FONT_ID, layout.title.x, titleY, chapter.c_str());
  }

  if (summary.hasProgress && layout.progress.height > 0) {
    if (layout.progressBar.width > 0 && layout.progressBar.height > 0) {
      renderer.drawRect(layout.progressBar.x, layout.progressBar.y, layout.progressBar.width,
                        layout.progressBar.height);
      const int fillWidth = DashboardProgress::fillWidth(layout.progressBar.width, summary.progressPercent);
      if (fillWidth > 0 && layout.progressBar.height > 4) {
        renderer.fillRect(layout.progressBar.x + 2, layout.progressBar.y + 2, fillWidth, layout.progressBar.height - 4);
      }
    }
    snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(summary.progressPercent));
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, value);
    renderer.drawText(SMALL_FONT_ID, layout.progressLabel.x + std::max(0, layout.progressLabel.width - labelWidth),
                      layout.progressLabel.y, value);
  } else if (layout.progress.height > 0) {
    const char* state = tr(STR_STATS_UNAVAILABLE);
    if (summary.progressState == DashboardMetricState::NoData) {
      state = tr(STR_DASH_NO_READING_DATA);
    } else if (summary.progressState == DashboardMetricState::NotTracked) {
      state = tr(STR_DASH_NOT_TRACKED);
    }
    const std::string progressText = std::string(tr(STR_STATS_PROGRESS)) + ": " + state;
    const std::string progress = renderer.truncatedText(SMALL_FONT_ID, progressText.c_str(), layout.progress.width);
    renderer.drawText(SMALL_FONT_ID, layout.progress.x, layout.progress.y, progress.c_str());
  }

  if (layout.footer.height > 0) {
    char scope[64]{};
    if (summary.usingSyncedStats) {
      snprintf(scope, sizeof(scope), tr(STR_STATS_ALL_SYNCED_FORMAT), static_cast<unsigned>(summary.syncedDeviceCount));
    } else {
      snprintf(scope, sizeof(scope), "%s", tr(STR_STATS_THIS_DEVICE));
    }

    std::string footerText = std::string(scope) + ": ";
    if (summary.globalStatsState == DashboardMetricState::Available) {
      formatDuration(summary.globalReadingSeconds, duration, sizeof(duration));
      footerText +=
          std::string(duration) + " | " + tr(STR_DASH_PAGES) + ": " + std::to_string(summary.globalPagesTurned);
    } else if (summary.globalStatsState == DashboardMetricState::NoData) {
      footerText += tr(STR_DASH_NO_READING_DATA);
    } else {
      footerText += tr(STR_STATS_UNAVAILABLE);
    }
    const std::string footer = renderer.truncatedText(SMALL_FONT_ID, footerText.c_str(), layout.footer.width);
    renderer.drawText(SMALL_FONT_ID, layout.footer.x, layout.footer.y, footer.c_str());

    const int secondLineY = layout.footer.y + renderer.getLineHeight(SMALL_FONT_ID);
    if (secondLineY + renderer.getLineHeight(SMALL_FONT_ID) <= layout.footer.y + layout.footer.height) {
      if (summary.syncedStatsState == DashboardMetricState::Unavailable &&
          summary.globalStatsState != DashboardMetricState::Unavailable) {
        const std::string warning =
            renderer.truncatedText(SMALL_FONT_ID, tr(STR_DASH_SYNC_UNAVAILABLE), layout.footer.width);
        renderer.drawText(SMALL_FONT_ID, layout.footer.x, secondLineY, warning.c_str());
      } else if (summary.hasCurrentStreak) {
        snprintf(value, sizeof(value), "%s: %u", tr(STR_STATS_STREAK), static_cast<unsigned>(summary.currentStreak));
        const std::string streak = renderer.truncatedText(SMALL_FONT_ID, value, layout.footer.width);
        renderer.drawText(SMALL_FONT_ID, layout.footer.x, secondLineY, streak.c_str());
      }
    }
  }
}

void DashboardTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount,
                                    const int selectedIndex, const std::function<std::string(int index)>& buttonLabel,
                                    const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  constexpr int COLUMNS = 2;
  constexpr int GAP = 8;
  constexpr int SIDE_PADDING = 20;
  const int tileWidth = (rect.width - SIDE_PADDING * 2 - GAP) / COLUMNS;
  const int rowHeight = DashboardMetrics::values.menuRowHeight;

  for (int i = 0; i < buttonCount; ++i) {
    const int column = i % COLUMNS;
    const int row = i / COLUMNS;
    const Rect tile{rect.x + SIDE_PADDING + column * (tileWidth + GAP),
                    rect.y + row * (rowHeight + DashboardMetrics::values.menuSpacing), tileWidth, rowHeight};
    const bool selected = i == selectedIndex;
    if (selected) {
      renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, CORNER_RADIUS, Color::LightGray);
    } else {
      renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 1, CORNER_RADIUS, true, true, true, true, true);
    }
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, buttonLabel(i).c_str(), tile.width - 20);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    const int textY = tile.y + (tile.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, tile.x + (tile.width - textWidth) / 2, textY, label.c_str());
  }
}
