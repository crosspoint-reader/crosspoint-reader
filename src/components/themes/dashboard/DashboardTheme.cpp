#include "DashboardTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "RecentBooksStore.h"
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

  char duration[24];
  char value[24];
  formatDuration(summary.bookReadingSeconds, duration, sizeof(duration));
  const int statsLineHeight = 42;
  int statsY = layout.stats.y + 4;
  renderer.drawText(UI_12_FONT_ID, layout.stats.x, statsY, tr(STR_DASH_READING_LEDGER), true, EpdFontFamily::BOLD);
  statsY += 45;
  drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_READING_TIME), duration);
  statsY += statsLineHeight;
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(summary.bookPagesTurned));
  drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_PAGES), value);
  statsY += statsLineHeight;
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(summary.bookSessions));
  drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_SESSIONS), value);
  statsY += statsLineHeight;
  if (summary.estimatedTimeLeftSeconds > 0) {
    formatDuration(summary.estimatedTimeLeftSeconds, duration, sizeof(duration));
    drawValueRow(renderer, layout.stats.x, statsY, layout.stats.width, tr(STR_DASH_TIME_LEFT), duration);
  }

  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), layout.title.width, 2, EpdFontFamily::BOLD);
  int titleY = layout.title.y;
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, layout.title.x, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += renderer.getLineHeight(UI_12_FONT_ID);
  }

  if (summary.hasProgress && layout.progress.height > 0) {
    drawProgressBar(renderer, layout.progress, summary.progressPercent, 100);
  }

  if (layout.footer.height > 0) {
    formatDuration(summary.globalReadingSeconds, duration, sizeof(duration));
    const std::string footerText = std::string(tr(STR_DASH_ALL_TIME)) + ": " + duration + "   " + tr(STR_DASH_PAGES) +
                                   ": " + std::to_string(summary.globalPagesTurned);
    const std::string footer = renderer.truncatedText(SMALL_FONT_ID, footerText.c_str(), layout.footer.width);
    renderer.drawText(SMALL_FONT_ID, layout.footer.x, layout.footer.y, footer.c_str());
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
