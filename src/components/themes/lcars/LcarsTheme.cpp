#include "LcarsTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// File-scope alias so method bodies can reference LcarsMetrics concisely.
namespace Lm = LcarsMetrics;

// ─────────────────────────────────────────────────────────────────────────────
// Internal constants (all in device pixels, portrait 480×800)
// ─────────────────────────────────────────────────────────────────────────────
namespace {
namespace M = LcarsMetrics;

// Convenience aliases so the code below stays readable
constexpr int LEFT_W   = M::LEFT_BAR_W;
constexpr int TOP_H    = M::TOP_BAR_H;
constexpr int OUT_R    = M::OUTER_RADIUS;
constexpr int NAV_GAP  = M::NAV_GAP;

// Bottom-bar geometry
constexpr int BTN_COUNT       = 4;
constexpr int BTN_OUTER_GAP_X = 6;   // gap between adjacent bottom-bar buttons (px)
constexpr int BTN_MARGIN_Y    = 7;   // top/bottom inset within the 44px bottom bar
constexpr int BTN_LABEL_Y_OFF = 5;   // baseline offset from top of button box

// List / content constants
constexpr int SEL_BAR_W  = 4;   // left-edge indicator width for selected list row
constexpr int LIST_PAD_X = 10;  // horizontal padding inside list rows
constexpr int CORNER_R   = 3;   // rounded corner radius for list selection highlight

// Tab bar
constexpr int TAB_PAD_X  = 12;  // horizontal padding inside each tab
constexpr int TAB_H      = Lm::values.tabBarHeight;

// Pop-up
constexpr int POPUP_MARGIN_X = 20;
constexpr int POPUP_MARGIN_Y = 14;
constexpr int POPUP_Y        = 200;  // y anchor for popup (within content area)

// ─── Helper: shift a full-screen Rect into the LCARS content area ────────────
// Activities pass Rect{0, y, pageWidth, h}; content must start at LEFT_W.
inline Rect contentRect(const Rect& r) {
  return Rect(r.x + LEFT_W, r.y, r.width - LEFT_W, r.height);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// drawFrame  –  Paints the entire LCARS chrome onto a freshly cleared screen.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawFrame(const GfxRenderer& renderer, const char* title) const {
  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  // ── 1. Left bar (full height, outer corners rounded on the left side) ──────
  renderer.fillRoundedRect(0, 0, LEFT_W, H, OUT_R,
                           /*tl=*/true, /*tr=*/false, /*bl=*/true, /*br=*/false,
                           Color::Black);

  // ── 2. Top bar strip ────────────────────────────────────────────────────────
  renderer.fillRect(LEFT_W, 0, W - LEFT_W, TOP_H, /*black=*/true);

  // ── 3. Bottom bar strip ─────────────────────────────────────────────────────
  renderer.fillRect(LEFT_W, H - TOP_H, W - LEFT_W, TOP_H, /*black=*/true);

  // ── 4. Nav segment decorators in the left bar ───────────────────────────────
  // Horizontal white separator lines divide the straight zone into segments,
  // giving the bar a classic LCARS multi-button appearance.
  {
    constexpr int NUM_SEGS  = 4;
    constexpr int LINE_INSET = 8;
    const int zoneTop    = NAV_GAP + 4;
    const int zoneBottom = H - NAV_GAP - 4;
    const int zoneH      = zoneBottom - zoneTop;
    for (int i = 1; i < NUM_SEGS; i++) {
      const int lineY = zoneTop + zoneH * i / NUM_SEGS;
      renderer.drawLine(LINE_INSET, lineY, LEFT_W - LINE_INSET, lineY, /*black=*/false);
    }
  }

  // ── 7. Battery percentage in left bar (white, bottom-aligned) ───────────────
  {
    const bool showPct =
        SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    if (showPct) {
      const uint16_t pct    = powerManager.getBatteryPercentage();
      const std::string txt = std::to_string(pct) + "%";
      const int txtW        = renderer.getTextWidth(SMALL_FONT_ID, txt.c_str());
      const int txtH        = renderer.getTextHeight(SMALL_FONT_ID);
      const int txtX        = (LEFT_W - txtW) / 2;
      const int txtY        = H - TOP_H - txtH - 6;
      renderer.drawText(SMALL_FONT_ID, txtX, txtY, txt.c_str(), /*black=*/false);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawHeader  –  Called once per render by each activity.
// Draws the complete LCARS chrome regardless of the rect passed.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawHeader(const GfxRenderer& renderer, Rect /*rect*/, const char* title,
                            const char* /*subtitle*/) const {
  drawFrame(renderer, title);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawSubHeader  –  Solid black band with white label text.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                                const char* rightLabel) const {
  const Rect cr = contentRect(rect);

  // Solid black background across the content width
  renderer.fillRect(cr.x, cr.y, cr.width, cr.height, /*black=*/true);

  // White label text, left-aligned with a small inset
  const int textH = renderer.getTextHeight(LCARS_10_FONT_ID);
  const int textY = cr.y + (cr.height - textH) / 2;
  if (label != nullptr) {
    const int maxW  = cr.width - LIST_PAD_X * 2 - (rightLabel != nullptr ? 80 : 0);
    const auto trunc = renderer.truncatedText(LCARS_10_FONT_ID, label, maxW, EpdFontFamily::BOLD);
    renderer.drawText(LCARS_10_FONT_ID, cr.x + LIST_PAD_X, textY, trunc.c_str(),
                      /*black=*/false, EpdFontFamily::BOLD);
  }

  // Optional right-aligned label (also white)
  if (rightLabel != nullptr) {
    const int rw    = renderer.getTextWidth(SMALL_FONT_ID, rightLabel);
    const int rh    = renderer.getTextHeight(SMALL_FONT_ID);
    const int ry    = cr.y + (cr.height - rh) / 2;
    renderer.drawText(SMALL_FONT_ID, cr.x + cr.width - LIST_PAD_X - rw, ry, rightLabel,
                      /*black=*/false);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawTabBar  –  Draws tab pills directly inside the LCARS top bar.
// The top bar is already filled black by drawFrame.
// Selected tab: white filled pill, black text.
// Others: white text only on the black bar.
// Tab labels are abbreviated to 4 uppercase chars to fit the narrow bar.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawTabBar(const GfxRenderer& renderer, Rect /*rect*/,
                             const std::vector<TabInfo>& tabs, bool /*selected*/) const {
  if (tabs.empty()) return;

  const int W         = renderer.getScreenWidth();
  const int tabCount  = static_cast<int>(tabs.size());
  // Reserve ~40px on the right for battery % in the left bar; tabs fill the rest of the top bar
  const int tabAreaW  = W - LEFT_W - 4;
  const int tabW      = tabAreaW / tabCount;
  const int barH      = TOP_H;

  for (int i = 0; i < tabCount; i++) {
    // Abbreviate label: uppercase, max 4 chars
    const char* full = tabs[i].label;
    char abbr[5] = {0};
    for (int c = 0; c < 4 && full[c] != '\0'; c++) {
      abbr[c] = (full[c] >= 'a' && full[c] <= 'z') ? full[c] - 32 : full[c];
    }

    const int tx = LEFT_W + i * tabW;
    const int tw = renderer.getTextWidth(LCARS_10_FONT_ID, abbr);
    const int th = renderer.getTextHeight(LCARS_10_FONT_ID);
    const int ty = (barH - th) / 2;

    if (tabs[i].selected) {
      // White filled pill with black text
      constexpr int PAD_X = 6, PAD_Y = 5;
      renderer.fillRect(tx + 2, PAD_Y, tabW - 4, barH - PAD_Y * 2, /*black=*/false);
      renderer.drawText(LCARS_10_FONT_ID, tx + (tabW - tw) / 2, ty, abbr, /*black=*/true);
    } else {
      // White text on black bar
      renderer.drawText(LCARS_10_FONT_ID, tx + (tabW - tw) / 2, ty, abbr, /*black=*/false);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawList  –  Paged list of items with optional subtitle / value / icon.
// Selected row: light-gray fill + left-edge indicator bar.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount,
                           int selectedIndex,
                           const std::function<std::string(int)>& rowTitle,
                           const std::function<std::string(int)>& rowSubtitle,
                           const std::function<UIIcon(int)>& rowIcon,
                           const std::function<std::string(int)>& rowValue,
                           bool highlightValue) const {
  const Rect cr      = contentRect(rect);
  const int rowH     = (rowSubtitle != nullptr) ? Lm::values.listWithSubtitleRowHeight
                                                : Lm::values.listRowHeight;
  const int pageItems = cr.height / rowH;
  const int totalPages = (itemCount + pageItems - 1) / pageItems;

  // ── Scroll indicator (arrows at right edge of content) ────────────────────
  if (totalPages > 1) {
    constexpr int arrowSz = 6;
    constexpr int arrowX  = 6;  // offset from right edge of content
    const int cx = cr.x + cr.width - arrowX;
    // Up arrow
    for (int i = 0; i < arrowSz; i++) {
      renderer.drawLine(cx - i, cr.y + i, cx + i, cr.y + i);
    }
    // Down arrow
    const int arrowBot = cr.y + cr.height - 1;
    for (int i = 0; i < arrowSz; i++) {
      const int lw = arrowSz - i;
      renderer.drawLine(cx - lw, arrowBot - i, cx + lw, arrowBot - i);
    }
  }

  // Reduce content width to leave room for scroll indicator
  const int contentW =
      cr.width - (totalPages > 1 ? (Lm::values.scrollBarWidth + Lm::values.scrollBarRightOffset + 4)
                                 : 0);
  const int maxValueW = 120;

  // ── Draw items on the current page ────────────────────────────────────────
  const int pageStart = (selectedIndex >= 0) ? (selectedIndex / pageItems) * pageItems : 0;

  for (int i = pageStart; i < itemCount && i < pageStart + pageItems; i++) {
    const int itemY = cr.y + (i % pageItems) * rowH;
    const bool isSel = (i == selectedIndex);

    // Selection highlight: light-gray row fill + left edge bar
    if (isSel) {
      renderer.fillRectDither(cr.x, itemY, contentW, rowH, Color::LightGray);
      renderer.fillRect(cr.x, itemY, SEL_BAR_W, rowH, /*black=*/true);
    }

    // Row separator (thin black line at bottom of row)
    renderer.drawLine(cr.x + SEL_BAR_W + LIST_PAD_X, itemY + rowH - 1,
                      cr.x + contentW - 1, itemY + rowH - 1);

    // Value text (right-aligned), computed before title to know available width
    int valueW = 0;
    std::string valueStr;
    if (rowValue != nullptr) {
      valueStr = rowValue(i);
      valueStr = renderer.truncatedText(LCARS_10_FONT_ID, valueStr.c_str(), maxValueW);
      valueW   = renderer.getTextWidth(LCARS_10_FONT_ID, valueStr.c_str()) + LIST_PAD_X;
    }

    // Title text
    const int titleX  = cr.x + SEL_BAR_W + LIST_PAD_X;
    const int titleMaxW = contentW - SEL_BAR_W - LIST_PAD_X * 2 - valueW;
    const int titleH  = renderer.getTextHeight(LCARS_10_FONT_ID);
    const int titleY  = rowSubtitle != nullptr ? itemY + 5
                                               : itemY + (rowH - titleH) / 2;

    const auto titleTrunc = renderer.truncatedText(LCARS_10_FONT_ID, rowTitle(i).c_str(), titleMaxW);
    renderer.drawText(LCARS_10_FONT_ID, titleX, titleY, titleTrunc.c_str(), /*black=*/true);

    // Subtitle text
    if (rowSubtitle != nullptr) {
      const auto sub = renderer.truncatedText(SMALL_FONT_ID, rowSubtitle(i).c_str(), titleMaxW);
      renderer.drawText(SMALL_FONT_ID, titleX, itemY + titleH + 8, sub.c_str(), /*black=*/true);
    }

    // Value text (draw right-aligned)
    if (!valueStr.empty()) {
      const int vx = cr.x + contentW - renderer.getTextWidth(LCARS_10_FONT_ID, valueStr.c_str()) -
                     LIST_PAD_X;
      const int vy = itemY + (rowH - renderer.getTextHeight(LCARS_10_FONT_ID)) / 2;
      if (isSel && highlightValue) {
        const int boxW = renderer.getTextWidth(LCARS_10_FONT_ID, valueStr.c_str()) + LIST_PAD_X * 2;
        renderer.fillRect(vx - LIST_PAD_X, itemY, boxW, rowH, /*black=*/true);
        renderer.drawText(LCARS_10_FONT_ID, vx, vy, valueStr.c_str(), /*black=*/false);
      } else {
        renderer.drawText(LCARS_10_FONT_ID, vx, vy, valueStr.c_str(), /*black=*/true);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawButtonHints  –  Action button labels in the bottom bar.
// The bottom bar is already filled black by drawFrame.  We draw white-outlined
// boxes for non-empty labels and white text inside them.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2,
                                  const char* btn3, const char* btn4) const {
  const GfxRenderer::Orientation origOri = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int W       = renderer.getScreenWidth();
  const int H       = renderer.getScreenHeight();
  const int barY    = H - TOP_H;
  const int barH    = TOP_H;
  const int avail   = W - LEFT_W;
  const int totalGap = BTN_OUTER_GAP_X * (BTN_COUNT - 1);
  const int btnW    = (avail - totalGap) / BTN_COUNT;

  const char* labels[BTN_COUNT] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < BTN_COUNT; i++) {
    const int x    = LEFT_W + i * (btnW + BTN_OUTER_GAP_X);
    const int boxY = barY + BTN_MARGIN_Y;
    const int boxH = barH - BTN_MARGIN_Y * 2;

    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // White-outlined box
      renderer.drawRect(x, boxY, btnW, boxH, /*black=*/false);
      // White centered label
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int tx = x + (btnW - tw) / 2;
      const int ty = boxY + BTN_LABEL_Y_OFF;
      renderer.drawText(SMALL_FONT_ID, tx, ty, labels[i], /*black=*/false);
    }
    // No box drawn for empty labels
  }

  renderer.setOrientation(origOri);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawSideButtonHints  –  Not visible in LCARS (left bar serves this role).
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawSideButtonHints(const GfxRenderer& /*renderer*/, const char* /*topBtn*/,
                                      const char* /*bottomBtn*/) const {
  // Intentionally empty: LCARS left bar provides navigation context visually.
}

// ─────────────────────────────────────────────────────────────────────────────
// drawButtonMenu  –  Home screen menu tiles in a 2-column grid.
// Selected tile: light-gray fill + black border + black text (consistent with list selection).
// Others: outlined box, black text.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawButtonMenu(GfxRenderer& renderer, Rect /*rect*/, int buttonCount,
                                 int selectedIndex,
                                 const std::function<std::string(int)>& buttonLabel,
                                 const std::function<UIIcon(int)>& /*rowIcon*/) const {
  // Single horizontal row of buttons, pinned just above the bottom bar.
  const int W        = renderer.getScreenWidth();
  const int H        = renderer.getScreenHeight();
  constexpr int GAP  = 4;
  constexpr int PAD  = Lm::values.contentSidePadding;
  const int tileH    = Lm::values.menuRowHeight;
  const int rowW     = W - LEFT_W - PAD * 2;
  const int tileW    = (rowW - GAP * (buttonCount - 1)) / buttonCount;
  const int rowY     = H - TOP_H - tileH - 8;

  for (int i = 0; i < buttonCount; i++) {
    const int tx   = LEFT_W + PAD + i * (tileW + GAP);
    const bool sel = (i == selectedIndex);

    if (sel) {
      renderer.fillRectDither(tx, rowY, tileW, tileH, Color::LightGray);
    }
    renderer.drawRect(tx, rowY, tileW, tileH);

    const std::string labelStr = buttonLabel(i);
    const int lw = renderer.getTextWidth(LCARS_10_FONT_ID, labelStr.c_str());
    const int lh = renderer.getTextHeight(LCARS_10_FONT_ID);
    renderer.drawText(LCARS_10_FONT_ID, tx + (tileW - lw) / 2, rowY + (tileH - lh) / 2,
                      labelStr.c_str(), /*black=*/true);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawRecentBookCover  –  Home screen recent books panel.
// Horizontal row layout: cover thumbnail on left, title + author on right.
// Covers are loaded from SD on the first render and saved to the frame buffer
// via storeCoverBuffer so subsequent renders only redraw selection / text.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                      const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered,
                                      bool& coverBufferStored, bool& /*bufferRestored*/,
                                      std::function<bool()> storeCoverBuffer) const {
  const Rect cr      = contentRect(rect);
  const int maxBooks = Lm::values.homeRecentBooksCount;
  const int count    = std::min(static_cast<int>(recentBooks.size()), maxBooks);
  const int rowH     = cr.height / maxBooks;

  if (count == 0) {
    const int msgY = cr.y + cr.height / 2 - renderer.getTextHeight(LCARS_10_FONT_ID);
    renderer.drawCenteredText(LCARS_10_FONT_ID, msgY, tr(STR_NO_OPEN_BOOK), /*black=*/true);
    renderer.drawCenteredText(LCARS_10_FONT_ID, msgY + renderer.getTextHeight(LCARS_10_FONT_ID) + 4,
                              tr(STR_START_READING), /*black=*/true);
    coverRendered = true;
    return;
  }

  // Cover thumbnail geometry – centered vertically within each row
  const int coverH   = Lm::values.homeCoverHeight;  // stored BMP height
  const int COVER_W  = coverH * 2 / 3;              // fixed layout width (~2:3 aspect ratio)
  const int topInset = (rowH - coverH) / 2;         // vertical centering inset

  const int thumbX    = cr.x + SEL_BAR_W + LIST_PAD_X;  // cover left edge (constant per column)
  const int thumbEndX = thumbX + COVER_W;

  // ── Phase 1: Load cover bitmaps from SD (first render only) ──────────────
  if (!coverRendered) {
    for (int i = 0; i < count; i++) {
      const RecentBook& book = recentBooks[i];
      const int thumbY = cr.y + i * rowH + topInset;

      bool hasCover = !book.coverBmpPath.empty();
      if (hasCover) {
        const std::string bmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverH);
        FsFile file;
        if (Storage.openFileForRead("LCARS_HOME", bmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            renderer.drawBitmap(bitmap, thumbX, thumbY, COVER_W, coverH);
          } else {
            hasCover = false;
          }
          file.close();
        } else {
          hasCover = false;
        }
      }

      // Cover outline (always drawn)
      renderer.drawRect(thumbX, thumbY, COVER_W, coverH, /*black=*/true);

      // Placeholder icon when no cover available
      if (!hasCover) {
        renderer.drawIcon(CoverIcon, thumbX + (COVER_W - 32) / 2, thumbY + (coverH - 32) / 2,
                          32, 32);
      }
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = true;
  }

  // ── Phase 2: Draw selection highlight + text (every render) ──────────────
  // Grey is drawn AROUND the cover box (not over it) so the cover image from
  // the restored frame buffer stays visible on the selected row.
  for (int i = 0; i < count; i++) {
    const RecentBook& book = recentBooks[i];
    const int rowY       = cr.y + i * rowH;
    const int coverStartY = rowY + topInset;
    const int coverEndY   = coverStartY + coverH;
    const bool isSel      = (i == selectorIndex);

    if (isSel) {
      // Top inset strip (above cover, full row width)
      renderer.fillRectDither(cr.x + SEL_BAR_W, rowY,
                              cr.width - SEL_BAR_W, topInset, Color::LightGray);
      // Bottom inset strip (below cover, full row width)
      renderer.fillRectDither(cr.x + SEL_BAR_W, coverEndY,
                              cr.width - SEL_BAR_W, topInset, Color::LightGray);
      // Left gap between selector bar and cover (cover height only)
      renderer.fillRectDither(cr.x + SEL_BAR_W, coverStartY,
                              LIST_PAD_X, coverH, Color::LightGray);
      // Text area right of cover (full row height)
      renderer.fillRectDither(thumbEndX, rowY,
                              cr.x + cr.width - thumbEndX, rowH, Color::LightGray);
      // Left indicator bar
      renderer.fillRect(cr.x, rowY, SEL_BAR_W, rowH, /*black=*/true);
    }

    // Row separator line
    renderer.drawLine(cr.x + SEL_BAR_W, rowY + rowH - 1, cr.x + cr.width - 1, rowY + rowH - 1);

    // Title + author in the text column (right of cover)
    const int textX    = thumbEndX + LIST_PAD_X;
    const int textMaxW = cr.x + cr.width - textX - LIST_PAD_X;
    const int titleH   = renderer.getTextHeight(LCARS_10_FONT_ID);
    const int authH    = renderer.getTextHeight(SMALL_FONT_ID);
    const int textY    = rowY + (rowH - titleH - 4 - authH) / 2;

    const auto titleTrunc =
        renderer.truncatedText(LCARS_10_FONT_ID, book.title.c_str(), textMaxW, EpdFontFamily::BOLD);
    renderer.drawText(LCARS_10_FONT_ID, textX, textY, titleTrunc.c_str(), /*black=*/true,
                      EpdFontFamily::BOLD);

    if (!book.author.empty()) {
      const auto authTrunc = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), textMaxW);
      renderer.drawText(SMALL_FONT_ID, textX, textY + titleH + 4, authTrunc.c_str(), /*black=*/true);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawProgressBar  –  LCARS-style segmented progress bar (e.g. downloads).
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current,
                                  size_t total) const {
  if (total == 0) return;

  const Rect cr  = contentRect(rect);
  const int pct  = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  constexpr int SEG_COUNT = 20;
  constexpr int SEG_GAP   = 2;
  const int segW          = (cr.width - SEG_GAP * (SEG_COUNT - 1)) / SEG_COUNT;
  const int filledSegs    = pct * SEG_COUNT / 100;

  for (int s = 0; s < SEG_COUNT; s++) {
    const int sx = cr.x + s * (segW + SEG_GAP);
    if (s < filledSegs) {
      renderer.fillRect(sx, cr.y, segW, cr.height, /*black=*/true);
    } else {
      renderer.drawRect(sx, cr.y, segW, cr.height);
    }
  }

  // Percentage label centered below bar
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  renderer.drawCenteredText(LCARS_10_FONT_ID, cr.y + cr.height + 10, pctBuf);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawReadingProgressBar  –  Thin progress strip along the top of the content
// area (visible above the book text, below the black top bar).
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawReadingProgressBar(const GfxRenderer& renderer,
                                         const size_t bookProgress) const {
  const int contentW = renderer.getScreenWidth() - LEFT_W;
  const int barH     = Lm::values.bookProgressBarHeight;
  const int barY     = TOP_H;  // immediately below the top bar
  const int fillW    = contentW * static_cast<int>(bookProgress) / 100;
  if (fillW > 0) {
    renderer.fillRect(LEFT_W, barY, fillW, barH, /*black=*/true);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawPopup  –  Centered modal message box inside the content area.
// ─────────────────────────────────────────────────────────────────────────────
Rect LcarsTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const int W       = renderer.getScreenWidth();
  const int textW   = renderer.getTextWidth(UI_12_FONT_ID, message);
  const int textH   = renderer.getLineHeight(UI_12_FONT_ID);
  const int boxW    = textW + POPUP_MARGIN_X * 2;
  const int boxH    = textH + POPUP_MARGIN_Y * 2;
  const int boxX    = LEFT_W + (W - LEFT_W - boxW) / 2;
  const int boxY    = POPUP_Y;
  constexpr int OUT = 2;  // outline thickness

  // White frame, then black fill
  renderer.fillRect(boxX - OUT, boxY - OUT, boxW + OUT * 2, boxH + OUT * 2, /*black=*/false);
  renderer.fillRect(boxX, boxY, boxW, boxH, /*black=*/true);

  const int tx = boxX + (boxW - textW) / 2;
  const int ty = boxY + POPUP_MARGIN_Y - 2;
  renderer.drawText(UI_12_FONT_ID, tx, ty, message, /*black=*/false);
  renderer.displayBuffer();

  return Rect{boxX, boxY, boxW, boxH};
}

// ─────────────────────────────────────────────────────────────────────────────
// fillPopupProgress  –  Fills an expanding progress line inside a popup.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout,
                                    int progress) const {
  constexpr int barH = 4;
  const int barW     = layout.width - POPUP_MARGIN_X * 2;
  const int barX     = layout.x + (layout.width - barW) / 2;
  const int barY     = layout.y + layout.height - POPUP_MARGIN_Y / 2 - barH / 2 - 1;
  const int fillW    = barW * progress / 100;

  renderer.fillRect(barX, barY, fillW, barH, /*black=*/false);  // white fill on black bg
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawTextField  –  Underline decoration for an active text input.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth) const {
  constexpr int PAD    = 8;
  const int lineW      = textWidth + PAD * 2;
  const int lineY      = rect.y + rect.height + renderer.getLineHeight(UI_12_FONT_ID) +
                         Lm::values.verticalSpacing;
  const int lineX      = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineX, lineY, lineX + lineW, lineY, /*lineWidth=*/2, /*black=*/true);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawKeyboardKey  –  Single keyboard key, filled when selected.
// ─────────────────────────────────────────────────────────────────────────────
void LcarsTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label,
                                  bool isSelected) const {
  constexpr int R = 3;
  if (isSelected) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, R, Color::Black);
  } else {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, R, /*black=*/true);
  }
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int th = renderer.getTextHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - tw) / 2,
                    rect.y + (rect.height - th) / 2, label, /*black=*/!isSelected);
}
