#include "CompactTheme.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/shared/ThemeShared.h"
#include "fontIds.h"

namespace {
constexpr int kStripCoverWidth = 34;
constexpr int kStripCoverHeight = 48;
constexpr int kStripCoverGap = 12;
constexpr int kStripPaddingTop = 8;

// Flip to true to spread the menu rows over the full available height instead of
// stacking them tightly at the top. Off by default to match the Compact spec.
constexpr bool kFillMenuHeight = false;
}  // namespace

void CompactTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  (void)subtitle;
  const auto& m = CompactMetrics::values;
  ThemeShared::drawTitleStatusBar(*this, renderer, rect, title, m.contentSidePadding, m.batteryWidth, m.batteryHeight,
                                  UI_10_FONT_ID);
}

void CompactTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)selectorIndex;  // Continue reading lives in the menu, so the strip is not selectable.

  const auto& m = CompactMetrics::values;

  if (bufferRestored && coverRendered) {
    return;
  }

  const int left = rect.x + m.contentSidePadding;
  const int right = rect.x + rect.width - m.contentSidePadding;
  const int ruleY = rect.y + rect.height - 1;

  if (recentBooks.empty()) {
    renderer.drawLine(left, ruleY, right - 1, ruleY, true);
    return;
  }

  const RecentBook& book = recentBooks[0];
  const int coverX = left;
  const int coverY = rect.y + kStripPaddingTop;

  bool haveBitmap = false;
  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, m.homeCoverHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bitmap, coverX, coverY, kStripCoverWidth, kStripCoverHeight);
        haveBitmap = true;
      }
    }
  }
  renderer.drawRect(coverX, coverY, kStripCoverWidth, kStripCoverHeight, 1, true);

  // Title and author stacked to the right of the cover.
  const int textLeft = coverX + kStripCoverWidth + kStripCoverGap;
  const int textWidth = right - textLeft;
  if (textWidth > 0) {
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

    const std::string title = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), textWidth, EpdFontFamily::BOLD);
    const std::string author =
        book.author.empty() ? std::string{} : renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);

    int blockHeight = titleLineHeight;
    if (!author.empty()) {
      blockHeight += authorLineHeight + 2;
    }

    int textY = coverY + (kStripCoverHeight - blockHeight) / 2;
    renderer.drawText(UI_12_FONT_ID, textLeft, textY, title.c_str(), true, EpdFontFamily::BOLD);
    textY += titleLineHeight + 2;
    if (!author.empty()) {
      renderer.drawText(UI_10_FONT_ID, textLeft, textY, author.c_str());
    }
  }

  renderer.drawLine(left, ruleY, right - 1, ruleY, true);

  if (haveBitmap && !coverRendered) {
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
}

void CompactTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;

  const auto& m = CompactMetrics::values;
  if (buttonCount <= 0) {
    return;
  }

  int rowStep = m.menuRowHeight + m.menuSpacing;
  if (kFillMenuHeight && rect.height > rowStep * buttonCount) {
    rowStep = rect.height / buttonCount;
  }
  const int rowHeight = kFillMenuHeight ? rowStep : m.menuRowHeight;

  const int textLeft = rect.x + m.contentSidePadding;
  const int textWidth = rect.width - m.contentSidePadding * 2;
  if (textWidth <= 0) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  for (int i = 0; i < buttonCount; ++i) {
    const int rowY = rect.y + i * rowStep;
    const bool selected = (i == selectedIndex);

    // Full-bleed inverted bar: runs edge to edge rather than inset, which is what
    // makes the row read as a bar instead of a button.
    if (selected) {
      renderer.fillRect(rect.x, rowY, rect.width, rowHeight, true);
    }

    const std::string label = renderer.truncatedText(UI_10_FONT_ID, buttonLabel(i).c_str(), textWidth);
    renderer.drawText(UI_10_FONT_ID, textLeft, rowY + (rowHeight - lineHeight) / 2, label.c_str(), !selected);

    // Hairline under every row, including the selected one.
    const int dividerY = rowY + rowHeight - 1;
    renderer.drawLine(textLeft, dividerY, rect.x + rect.width - m.contentSidePadding - 1, dividerY, !selected);
  }
}

void CompactTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  ThemeShared::drawFlatButtonHints(renderer, btn1, btn2, btn3, btn4);
}
