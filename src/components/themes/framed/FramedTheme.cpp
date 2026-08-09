#include "FramedTheme.h"

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
constexpr int kFrameInset = 8;
constexpr int kSelectionTabWidth = 4;
constexpr int kSelectionTabGap = 10;
constexpr int kTitleMaxLines = 2;
}  // namespace

void FramedTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  (void)subtitle;  // Framed has no subtitle lane; the status bar carries the title.

  const auto& m = FramedMetrics::values;

  // The frame is drawn here because drawHeader is the first theme call in
  // HomeActivity::render -- there is no dedicated frame hook in BaseTheme.
  const int frameX = kFrameInset;
  const int frameY = kFrameInset;
  const int frameW = renderer.getScreenWidth() - kFrameInset * 2;
  const int frameH = renderer.getScreenHeight() - m.buttonHintsHeight - kFrameInset * 2;
  if (frameW > 0 && frameH > 0) {
    renderer.drawRect(frameX, frameY, frameW, frameH, 1, true);
  }

  // Inset the status bar so it sits inside the frame rather than across it.
  const Rect band{rect.x + kFrameInset, rect.y, rect.width - kFrameInset * 2, rect.height};
  ThemeShared::drawTitleStatusBar(*this, renderer, band, title, m.contentSidePadding - kFrameInset, m.batteryWidth,
                                  m.batteryHeight, UI_10_FONT_ID);
}

void FramedTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)selectorIndex;  // Continue reading is a menu row here, so the tile is not selectable.

  const auto& m = FramedMetrics::values;

  // The whole tile -- cover, title, author, rule -- is inside the snapshot region
  // HomeActivity stores, so a successful restore means there is nothing to redraw.
  if (bufferRestored && coverRendered) {
    return;
  }

  const int innerLeft = rect.x + kFrameInset + m.contentSidePadding;
  const int innerWidth = rect.width - (kFrameInset + m.contentSidePadding) * 2;
  if (innerWidth <= 0) {
    return;
  }

  if (recentBooks.empty()) {
    // Nothing read yet: leave the tile empty apart from the closing rule, so the
    // menu below still lines up where the user expects it.
    const int ruleY = rect.y + rect.height - 1;
    renderer.drawLine(innerLeft, ruleY, innerLeft + innerWidth - 1, ruleY, true);
    return;
  }

  const RecentBook& book = recentBooks[0];

  const int lineHeightTitle = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineHeightAuthor = renderer.getLineHeight(UI_10_FONT_ID);

  const std::vector<std::string> titleLines =
      renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), innerWidth, kTitleMaxLines);
  const std::string author =
      book.author.empty() ? std::string{} : renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), innerWidth);

  // Reserve the text block first, then give whatever is left to the cover.
  int textBlockHeight = static_cast<int>(titleLines.size()) * lineHeightTitle + 8;
  if (!author.empty()) {
    textBlockHeight += lineHeightAuthor + 4;
  }
  const int ruleReserve = 12;
  const int coverBoxHeight = std::min(m.homeCoverHeight, rect.height - textBlockHeight - ruleReserve);

  int coverW = 0;
  int coverH = 0;
  bool haveBitmap = false;

  if (!book.coverBmpPath.empty() && coverBoxHeight > 0) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, m.homeCoverHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int imgW = bitmap.getWidth();
        const int imgH = bitmap.getHeight();
        if (imgW > 0 && imgH > 0) {
          coverH = coverBoxHeight;
          coverW = coverH * imgW / imgH;
          if (coverW > innerWidth) {
            coverW = innerWidth;
            coverH = coverW * imgH / imgW;
          }
          const int coverX = rect.x + (rect.width - coverW) / 2;
          renderer.drawBitmap(bitmap, coverX, rect.y, coverW, coverH);
          renderer.drawRect(coverX, rect.y, coverW, coverH, 1, true);
          haveBitmap = true;
        }
      }
    }
  }

  if (!haveBitmap && coverBoxHeight > 0) {
    // No cover art: an empty outlined card of roughly paperback proportions.
    coverH = coverBoxHeight;
    coverW = std::min(innerWidth, coverH * 2 / 3);
    const int coverX = rect.x + (rect.width - coverW) / 2;
    renderer.drawRect(coverX, rect.y, coverW, coverH, 1, true);
  }

  int textY = rect.y + coverH + 8;
  for (const std::string& line : titleLines) {
    const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - lineWidth) / 2, textY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    textY += lineHeightTitle;
  }

  if (!author.empty()) {
    textY += 4;
    const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, author.c_str());
    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - authorWidth) / 2, textY, author.c_str());
    textY += lineHeightAuthor;
  }

  const int ruleY = std::min(textY + 8, rect.y + rect.height - 1);
  renderer.drawLine(innerLeft, ruleY, innerLeft + innerWidth - 1, ruleY, true);

  if (haveBitmap && !coverRendered) {
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
}

void FramedTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                 const std::function<std::string(int index)>& buttonLabel,
                                 const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;  // Framed is a text-only menu.

  const auto& m = FramedMetrics::values;

  const int rowLeft = rect.x + kFrameInset + m.contentSidePadding;
  const int rowWidth = rect.width - (kFrameInset + m.contentSidePadding) * 2;
  const int textLeft = rowLeft + kSelectionTabWidth + kSelectionTabGap;
  const int textWidth = rowWidth - kSelectionTabWidth - kSelectionTabGap;
  if (textWidth <= 0) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  for (int i = 0; i < buttonCount; ++i) {
    const int rowY = rect.y + i * (m.menuRowHeight + m.menuSpacing);
    const bool selected = (i == selectedIndex);

    if (selected) {
      renderer.fillRect(rowLeft, rowY, kSelectionTabWidth, m.menuRowHeight, true);
    }

    const EpdFontFamily::Style style = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string label = renderer.truncatedText(UI_12_FONT_ID, buttonLabel(i).c_str(), textWidth, style);
    renderer.drawText(UI_12_FONT_ID, textLeft, rowY + (m.menuRowHeight - lineHeight) / 2, label.c_str(), true, style);
  }
}

void FramedTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                  const char* btn4) const {
  ThemeShared::drawFlatButtonHints(renderer, btn1, btn2, btn3, btn4, FramedMetrics::values.buttonHintsHeight);
}
