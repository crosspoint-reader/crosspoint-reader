#include "NewspaperTheme.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/shared/ThemeShared.h"
#include "fontIds.h"

namespace {
constexpr int kMastheadFontId = NOTOSERIF_18_FONT_ID;
constexpr int kHeadlineFontId = NOTOSERIF_16_FONT_ID;
constexpr int kAuthorFontId = NOTOSERIF_12_FONT_ID;
constexpr const char* kMasthead = "The Crosspoint";
constexpr int kHeavyRuleThickness = 3;
constexpr int kLeadCoverGap = 14;
constexpr int kHeadlineMaxLines = 3;
}  // namespace

void NewspaperTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  (void)subtitle;
  (void)title;  // The lead block carries the book title; the masthead is fixed.

  const auto& m = NewspaperMetrics::values;

  const int left = rect.x + m.contentSidePadding;
  const int right = rect.x + rect.width - m.contentSidePadding;
  const int width = right - left;
  if (width <= 0) {
    return;
  }

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  // Masthead.
  const int mastheadHeight = renderer.getLineHeight(kMastheadFontId);
  const int mastheadWidth = renderer.getTextWidth(kMastheadFontId, kMasthead, EpdFontFamily::BOLD);
  renderer.drawText(kMastheadFontId, rect.x + (rect.width - mastheadWidth) / 2, rect.y + 4, kMasthead, true,
                    EpdFontFamily::BOLD);

  // Double rule: 2px over a hairline, with a gap between them.
  int y = rect.y + 4 + mastheadHeight + 6;
  renderer.fillRect(left, y, width, 2, true);
  y += 5;
  renderer.drawLine(left, y, right - 1, y, true);

  // Band: recent-book count on the left, battery on the right.
  //
  // drawHeader only receives a title, and it runs before drawRecentBookCover, so the
  // count is read from the global store rather than the parameter list.
  const int bandTop = y + 3;
  const int bandHeight = std::max(0, rect.y + rect.height - bandTop - 6);

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int count = static_cast<int>(RECENT_BOOKS.getCount());
  // The label is the untouched i18n string; only the numeral is appended.
  const std::string countText = std::string(I18N.get(StrId::STR_MENU_RECENT_BOOKS)) + "  " + std::to_string(count);
  const std::string countFitted = renderer.truncatedText(SMALL_FONT_ID, countText.c_str(), width / 2);
  renderer.drawText(SMALL_FONT_ID, left, bandTop + (bandHeight - lineHeight) / 2, countFitted.c_str());

  const bool showPercent = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryY = bandTop + (bandHeight - m.batteryHeight) / 2;
  drawBatteryRight(renderer, Rect{right - m.batteryWidth, batteryY, m.batteryWidth, m.batteryHeight}, showPercent);

  // Closing hairline under the band.
  const int bandRuleY = rect.y + rect.height - 3;
  renderer.drawLine(left, bandRuleY, right - 1, bandRuleY, true);
}

void NewspaperTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)selectorIndex;  // Continue reading is a menu row; the lead block is not selectable.

  const auto& m = NewspaperMetrics::values;

  if (bufferRestored && coverRendered) {
    return;
  }

  const int left = rect.x + m.contentSidePadding;
  const int right = rect.x + rect.width - m.contentSidePadding;
  const int width = right - left;
  if (width <= 0) {
    return;
  }

  const int heavyRuleY = rect.y + rect.height - kHeavyRuleThickness - 4;

  if (recentBooks.empty()) {
    renderer.fillRect(left, heavyRuleY, width, kHeavyRuleThickness, true);
    return;
  }

  const RecentBook& book = recentBooks[0];

  const int coverH = std::min(m.homeCoverHeight, heavyRuleY - rect.y - 10);
  int coverW = coverH * 2 / 3;
  const int coverY = rect.y + 4;

  bool haveBitmap = false;
  if (!book.coverBmpPath.empty() && coverH > 0) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, m.homeCoverHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int imgW = bitmap.getWidth();
        const int imgH = bitmap.getHeight();
        if (imgW > 0 && imgH > 0) {
          coverW = coverH * imgW / imgH;
          // Already a 1-bit dithered thumbnail, so this is the halftone.
          renderer.drawBitmap(bitmap, left, coverY, coverW, coverH);
          haveBitmap = true;
        }
      }
    }
  }
  if (!haveBitmap && coverH > 0) {
    renderer.fillRectDither(left, coverY, coverW, coverH, Color::LightGray);
  }
  if (coverH > 0) {
    renderer.drawRect(left, coverY, coverW, coverH, 1, true);
  }

  // Serif headline and author to the right of the cover.
  const int textLeft = left + coverW + kLeadCoverGap;
  const int textWidth = right - textLeft;
  if (textWidth > 0) {
    const int headlineLineHeight = renderer.getLineHeight(kHeadlineFontId);
    const int authorLineHeight = renderer.getLineHeight(kAuthorFontId);

    const std::vector<std::string> headline =
        renderer.wrappedText(kHeadlineFontId, book.title.c_str(), textWidth, kHeadlineMaxLines);
    const std::string author =
        book.author.empty() ? std::string{} : renderer.truncatedText(kAuthorFontId, book.author.c_str(), textWidth);

    int textY = coverY;
    for (const std::string& line : headline) {
      renderer.drawText(kHeadlineFontId, textLeft, textY, line.c_str(), true, EpdFontFamily::BOLD);
      textY += headlineLineHeight;
    }
    if (!author.empty()) {
      textY += 6;
      renderer.drawText(kAuthorFontId, textLeft, textY, author.c_str());
      textY += authorLineHeight;
    }
  }

  renderer.fillRect(left, heavyRuleY, width, kHeavyRuleThickness, true);

  if (haveBitmap && !coverRendered) {
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
}

void NewspaperTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                    const std::function<std::string(int index)>& buttonLabel,
                                    const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;

  const auto& m = NewspaperMetrics::values;

  const int left = rect.x + m.contentSidePadding;
  const int right = rect.x + rect.width - m.contentSidePadding;
  const int textWidth = right - left;
  if (textWidth <= 0) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  for (int i = 0; i < buttonCount; ++i) {
    const int rowY = rect.y + i * (m.menuRowHeight + m.menuSpacing);
    const bool selected = (i == selectedIndex);

    if (selected) {
      renderer.fillRect(rect.x, rowY, rect.width, m.menuRowHeight, true);
    }

    const std::string label = renderer.truncatedText(UI_12_FONT_ID, buttonLabel(i).c_str(), textWidth);
    renderer.drawText(UI_12_FONT_ID, left, rowY + (m.menuRowHeight - lineHeight) / 2, label.c_str(), !selected);

    // Hairline between rows (not after the last one).
    if (i + 1 < buttonCount) {
      const int dividerY = rowY + m.menuRowHeight - 1;
      renderer.drawLine(left, dividerY, right - 1, dividerY, !selected);
    }
  }
}

void NewspaperTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                     const char* btn4) const {
  ThemeShared::drawFlatButtonHints(renderer, btn1, btn2, btn3, btn4);
}
