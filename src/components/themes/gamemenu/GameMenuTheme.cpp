#include "GameMenuTheme.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/shared/ThemeShared.h"
#include "fontIds.h"

namespace {
constexpr int kCardRadius = 8;
constexpr int kCardStroke = 2;
constexpr int kHeroPadding = 14;
constexpr int kHeroCoverGap = 16;
constexpr int kTitleMaxLines = 2;
}  // namespace

void GameMenuTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  (void)subtitle;
  const auto& m = GameMenuMetrics::values;
  ThemeShared::drawTitleStatusBar(*this, renderer, rect, title, m.contentSidePadding, m.batteryWidth, m.batteryHeight,
                                  UI_10_FONT_ID);
}

void GameMenuTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                        const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                        bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const auto& m = GameMenuMetrics::values;

  heroActive_ = !recentBooks.empty();
  if (!heroActive_) {
    // No recent book means HomeActivity did not insert a Continue reading row, so
    // there is no hero to draw and drawButtonMenu will render every entry itself.
    return;
  }

  const bool selected = (selectorIndex == 0);

  // The hero inverts on selection, so a restored snapshot is only reusable if the
  // selection state has not moved. Simplest correct thing: always repaint the hero.
  (void)bufferRestored;

  const int cardX = rect.x + m.contentSidePadding;
  const int cardW = rect.width - m.contentSidePadding * 2;
  const int cardH = rect.height - 12;
  if (cardW <= 0 || cardH <= 0) {
    return;
  }

  // Clear first: rounded corners leave the previous card's pixels outside the new
  // fill otherwise.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  if (selected) {
    renderer.fillRoundedRect(cardX, rect.y, cardW, cardH, kCardRadius, Color::Black);
  } else {
    renderer.drawRoundedRect(cardX, rect.y, cardW, cardH, kCardStroke, kCardRadius, true);
  }

  // Text is inverted when the card is filled.
  const bool ink = !selected;

  const RecentBook& book = recentBooks[0];

  const int coverH = cardH - kHeroPadding * 2;
  int coverW = coverH * 2 / 3;
  const int coverX = cardX + kHeroPadding;
  const int coverY = rect.y + kHeroPadding;

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
          renderer.drawBitmap(bitmap, coverX, coverY, coverW, coverH);
          haveBitmap = true;
        }
      }
    }
  }
  if (!haveBitmap) {
    renderer.drawRect(coverX, coverY, coverW, coverH, 1, ink);
  }

  const int textLeft = coverX + coverW + kHeroCoverGap;
  const int textWidth = cardX + cardW - kHeroPadding - textLeft;
  if (textWidth <= 0) {
    return;
  }

  // The label itself comes from i18n untouched -- a theme controls the boxes, not
  // the words, and reformatting would break CJK, Arabic and Hebrew.
  const char* continueLabel = I18N.get(StrId::STR_CONTINUE_READING);

  const int labelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  const std::vector<std::string> titleLines =
      renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, kTitleMaxLines);
  const std::string author =
      book.author.empty() ? std::string{} : renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);

  int blockHeight = labelLineHeight + 6 + static_cast<int>(titleLines.size()) * titleLineHeight;
  if (!author.empty()) {
    blockHeight += authorLineHeight + 4;
  }

  int textY = coverY + std::max(0, (coverH - blockHeight) / 2);

  const std::string label = renderer.truncatedText(UI_10_FONT_ID, continueLabel, textWidth);
  renderer.drawText(UI_10_FONT_ID, textLeft, textY, label.c_str(), ink);
  textY += labelLineHeight + 6;

  for (const std::string& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, textLeft, textY, line.c_str(), ink, EpdFontFamily::BOLD);
    textY += titleLineHeight;
  }

  if (!author.empty()) {
    textY += 4;
    renderer.drawText(UI_10_FONT_ID, textLeft, textY, author.c_str(), ink);
  }

  // The hero repaints every frame, so there is no point caching it. Leave the flags
  // as they are so HomeActivity keeps calling us.
  (void)coverRendered;
  (void)coverBufferStored;
  (void)storeCoverBuffer;
}

void GameMenuTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                   const std::function<std::string(int index)>& buttonLabel,
                                   const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;

  const auto& m = GameMenuMetrics::values;

  // Index 0 is Continue reading whenever a hero was drawn; skip it here.
  const int firstIndex = heroActive_ ? 1 : 0;

  const int cardX = rect.x + m.contentSidePadding;
  const int cardW = rect.width - m.contentSidePadding * 2;
  if (cardW <= 0) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  for (int i = firstIndex; i < buttonCount; ++i) {
    const int slot = i - firstIndex;
    const int cardY = rect.y + slot * (m.menuRowHeight + m.menuSpacing);
    const bool selected = (i == selectedIndex);

    if (selected) {
      renderer.fillRoundedRect(cardX, cardY, cardW, m.menuRowHeight, kCardRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(cardX, cardY, cardW, m.menuRowHeight, kCardStroke, kCardRadius, true);
    }

    const int textWidth = cardW - kHeroPadding * 2;
    if (textWidth <= 0) {
      continue;
    }
    const std::string label =
        renderer.truncatedText(UI_12_FONT_ID, buttonLabel(i).c_str(), textWidth, EpdFontFamily::BOLD);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, cardX + (cardW - labelWidth) / 2, cardY + (m.menuRowHeight - lineHeight) / 2,
                      label.c_str(), !selected, EpdFontFamily::BOLD);
  }
}

void GameMenuTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                    const char* btn4) const {
  ThemeShared::drawFlatButtonHints(renderer, btn1, btn2, btn3, btn4);
}
