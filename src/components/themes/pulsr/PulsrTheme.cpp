#include "PulsrTheme.h"

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

// File-scope alias so method bodies can reference PulsrMetrics concisely.
namespace Lm = PulsrMetrics;

// ─────────────────────────────────────────────────────────────────────────────
// Internal constants (all in device pixels, portrait 480×800)
// ─────────────────────────────────────────────────────────────────────────────
namespace {
namespace M = PulsrMetrics;

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

// ─── Helper: shift a full-screen Rect into the PULSR content area ────────────
// Activities pass Rect{0, y, pageWidth, h}; content must start at LEFT_W.
inline Rect contentRect(const Rect& r) {
  return Rect(r.x + LEFT_W, r.y, r.width - LEFT_W, r.height);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// drawFrame  –  Paints the entire PULSR chrome onto a freshly cleared screen.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawFrame(const GfxRenderer& renderer, const char* title) const {
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

  // ── 4-7. Left-bar segments — align with physical buttons (PWR / UP / DOWN / BATT)
  //
  //  The left bar is divided into 4 equal segments that visually line up with the
  //  three physical buttons on the right edge (Power, Up, Down) plus a battery segment.
  //
  //  Seg 0 (top)   → Power button  : PWR pill + HTTP pill (if server active) + FEED pill (if feed active)
  //  Seg 1         → Up button     : ↑ arrow label
  //  Seg 2         → Down button   : ↓ arrow label
  //  Seg 3 (bot)   → Battery       : vertical fill bar + percentage text
  {
    constexpr int NUM_SEGS   = 4;
    constexpr int LINE_INSET = 8;
    constexpr int SEG_MARGIN = 6;   // inset for pills/labels within each segment
    constexpr int PILL_R     = 5;   // pill corner radius
    constexpr int PILL_GAP   = 3;   // gap between stacked pills
    const int zoneTop    = NAV_GAP + 4;
    const int zoneBottom = H - NAV_GAP - 4;
    const int zoneH      = zoneBottom - zoneTop;
    const int segH       = zoneH / NUM_SEGS;
    const int pillW      = LEFT_W - SEG_MARGIN * 2;
    const int pillX      = SEG_MARGIN;

    // ── Separator lines between segments ──────────────────────────────────────
    for (int i = 1; i < NUM_SEGS; i++) {
      const int lineY = zoneTop + segH * i;
      renderer.drawLine(LINE_INSET, lineY, LEFT_W - LINE_INSET, lineY, /*black=*/false);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Seg 0: Power segment — PWR pill always present; HTTP + FEED when active.
    //        Pills stacked top-to-bottom with PILL_GAP between them.
    // ─────────────────────────────────────────────────────────────────────────
    {
      const int seg0Top = zoneTop;
      // Determine pill height: divide segment evenly among up to 3 pills
      constexpr int MAX_PILLS  = 3;
      const int totalGap = PILL_GAP * (MAX_PILLS - 1);
      const int pillH    = (segH - SEG_MARGIN * 2 - totalGap) / MAX_PILLS;

      // Helper lambda: draw one pill at the given slot index (0=top)
      // Returns the y of the next slot.
      int pillY = seg0Top + SEG_MARGIN;

      // PWR pill — always shown, grey background, black "PWR" label
      renderer.fillRoundedRect(pillX, pillY, pillW, pillH, PILL_R, Color::LightGray);
      {
        const char* lbl = "PWR";
        const int lw = renderer.getTextWidth(PULSR_10_FONT_ID, lbl);
        const int lh = renderer.getTextHeight(PULSR_10_FONT_ID);
        renderer.drawText(PULSR_10_FONT_ID, pillX + (pillW - lw) / 2, pillY + (pillH - lh) / 2, lbl, /*black=*/true);
      }
      pillY += pillH + PILL_GAP;

      // HTTP pill — only when web server is active
      if (UITheme::isHttpServerActive()) {
        const Color httpColor = UITheme::isNetworkTransferring()
            ? ((millis() / 600) % 2 == 0 ? Color::White : Color::LightGray)
            : Color::LightGray;
        renderer.fillRoundedRect(pillX, pillY, pillW, pillH, PILL_R, httpColor);
        const char* lbl = "HTTP";
        const int lw = renderer.getTextWidth(PULSR_10_FONT_ID, lbl);
        const int lh = renderer.getTextHeight(PULSR_10_FONT_ID);
        renderer.drawText(PULSR_10_FONT_ID, pillX + (pillW - lw) / 2, pillY + (pillH - lh) / 2, lbl, /*black=*/true);
      }
      pillY += pillH + PILL_GAP;

      // FEED pill — lit while feed is active (syncing or waiting), dark outline when done
      if (RssFeedSync::isFeedActive()) {
        const Color feedColor = RssFeedSync::isSyncing()
            ? ((millis() / 600) % 2 == 0 ? Color::White : Color::LightGray)
            : Color::LightGray;
        renderer.fillRoundedRect(pillX, pillY, pillW, pillH, PILL_R, feedColor);
        const char* lbl = "FEED";
        const int lw = renderer.getTextWidth(PULSR_10_FONT_ID, lbl);
        const int lh = renderer.getTextHeight(PULSR_10_FONT_ID);
        renderer.drawText(PULSR_10_FONT_ID, pillX + (pillW - lw) / 2, pillY + (pillH - lh) / 2, lbl, /*black=*/true);
      }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Seg 1: Up button — "↑" arrow centred in segment
    // ─────────────────────────────────────────────────────────────────────────
    {
      const int seg1CentreY = zoneTop + segH + segH / 2;
      const char* lbl = "^";   // up-arrow glyph (PULSR font)
      const int lw = renderer.getTextWidth(PULSR_12_FONT_ID, lbl);
      const int lh = renderer.getTextHeight(PULSR_12_FONT_ID);
      renderer.drawText(PULSR_12_FONT_ID, (LEFT_W - lw) / 2, seg1CentreY - lh / 2, lbl, /*black=*/false);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Seg 2: Down button — "↓" arrow centred in segment
    // ─────────────────────────────────────────────────────────────────────────
    {
      const int seg2CentreY = zoneTop + segH * 2 + segH / 2;
      const char* lbl = "v";   // down-arrow glyph (PULSR font)
      const int lw = renderer.getTextWidth(PULSR_12_FONT_ID, lbl);
      const int lh = renderer.getTextHeight(PULSR_12_FONT_ID);
      renderer.drawText(PULSR_12_FONT_ID, (LEFT_W - lw) / 2, seg2CentreY - lh / 2, lbl, /*black=*/false);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Seg 3: CHRG pill (when USB connected) + Battery bar + percentage
    // ─────────────────────────────────────────────────────────────────────────
    {
      const int seg3Top = zoneTop + segH * 3;
      const uint16_t pct = powerManager.getBatteryPercentage();
      const bool usbConnected = (digitalRead(20) == HIGH);

      // CHRG pill at top of segment
      constexpr int CHRG_PILL_H = 18;
      const int chrgY = seg3Top + SEG_MARGIN;
      if (usbConnected) {
        renderer.fillRoundedRect(pillX, chrgY, pillW, CHRG_PILL_H, PILL_R, Color::LightGray);
        const char* lbl = "CHRG";
        const int lw = renderer.getTextWidth(PULSR_10_FONT_ID, lbl);
        const int lh = renderer.getTextHeight(PULSR_10_FONT_ID);
        renderer.drawText(PULSR_10_FONT_ID, pillX + (pillW - lw) / 2,
                          chrgY + (CHRG_PILL_H - lh) / 2, lbl, /*black=*/true);
      }

      // Battery bar geometry — shifted down to make room for CHRG pill
      constexpr int BAR_MARGIN_X = 18;
      constexpr int BAR_MARGIN_BOT = 20;  // leave room for percentage text
      constexpr int BAR_R = 3;
      const int barTopOffset = SEG_MARGIN + CHRG_PILL_H + PILL_GAP;
      const int barX = BAR_MARGIN_X;
      const int barY = seg3Top + barTopOffset;
      const int barW = LEFT_W - BAR_MARGIN_X * 2;
      const int barH = segH - barTopOffset - BAR_MARGIN_BOT;

      // Outline
      renderer.drawRoundedRect(barX, barY, barW, barH, 1, BAR_R, /*black=*/false);

      // Fill from bottom: white = charged portion
      const int fillH = (barH * pct) / 100;
      if (fillH > 0) {
        renderer.fillRoundedRect(barX, barY + (barH - fillH), barW, fillH, BAR_R, Color::White);
      }

      // Percentage text centred below bar
      const std::string pctStr = std::to_string(pct) + "%";
      const int tw = renderer.getTextWidth(PULSR_10_FONT_ID, pctStr.c_str());
      const int th = renderer.getTextHeight(PULSR_10_FONT_ID);
      const int txtY = seg3Top + segH - BAR_MARGIN_BOT + (BAR_MARGIN_BOT - th) / 2;
      renderer.drawText(PULSR_10_FONT_ID, (LEFT_W - tw) / 2, txtY, pctStr.c_str(), /*black=*/false);
    }
  }

  // ── 6. Screen title in top bar (white, uppercase, PULSR-12) ────────────────
  {
    const char* raw = (title != nullptr) ? title : "HOME";
    std::string upper(raw);
    for (char& c : upper) c = (c >= 'a' && c <= 'z') ? c - 32 : c;
    const int maxW  = W - LEFT_W - 12;
    const auto lbl  = renderer.truncatedText(PULSR_12_FONT_ID, upper.c_str(), maxW);
    const int  txtH = renderer.getTextHeight(PULSR_12_FONT_ID);
    const int  txtY = (TOP_H - txtH) / 2;
    renderer.drawText(PULSR_12_FONT_ID, LEFT_W + 8, txtY, lbl.c_str(), /*black=*/false);
  }

  // ── 7. Battery now rendered in seg 3 of the left-bar block above ───────────
}

// ─────────────────────────────────────────────────────────────────────────────
// drawHeader  –  Called once per render by each activity.
// Draws the complete PULSR chrome regardless of the rect passed.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawHeader(const GfxRenderer& renderer, Rect /*rect*/, const char* title,
                            const char* /*subtitle*/) const {
  drawFrame(renderer, title);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawSubHeader  –  Solid black band with white label text.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                                const char* rightLabel) const {
  if (rect.height <= 0) return;  // tabBarHeight=0 in PULSR — nothing to draw
  const Rect cr = contentRect(rect);

  // Solid black background across the content width
  renderer.fillRect(cr.x, cr.y, cr.width, cr.height, /*black=*/true);

  // White label text, left-aligned with a small inset
  const int textH = renderer.getTextHeight(PULSR_10_FONT_ID);
  const int textY = cr.y + (cr.height - textH) / 2;
  if (label != nullptr) {
    const int maxW  = cr.width - LIST_PAD_X * 2 - (rightLabel != nullptr ? 80 : 0);
    const auto trunc = renderer.truncatedText(PULSR_10_FONT_ID, label, maxW, EpdFontFamily::BOLD);
    renderer.drawText(PULSR_10_FONT_ID, cr.x + LIST_PAD_X, textY, trunc.c_str(),
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
// drawTabBar  –  Draws tab pills directly inside the PULSR top bar.
// The top bar is already filled black by drawFrame.
// Selected tab: white filled pill, black text.
// Others: white text only on the black bar.
// Tab labels are abbreviated to 4 uppercase chars to fit the narrow bar.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawTabBar(const GfxRenderer& renderer, Rect /*rect*/,
                             const std::vector<TabInfo>& tabs, bool /*selected*/) const {
  if (tabs.empty()) return;

  const int W         = renderer.getScreenWidth();
  // Clear the top bar first — erase any screen title drawn by drawHeader
  renderer.fillRect(LEFT_W, 0, W - LEFT_W, TOP_H, /*black=*/true);

  const int tabCount  = static_cast<int>(tabs.size());
  // Tabs fill the rest of the top bar
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
    // "Controls" → "CONT" is ambiguous; override to the universal abbreviation
    if (abbr[0] == 'C' && abbr[1] == 'O' && abbr[2] == 'N' && abbr[3] == 'T') {
      abbr[0] = 'C'; abbr[1] = 'T'; abbr[2] = 'R'; abbr[3] = 'L';
    }

    const int tx = LEFT_W + i * tabW;
    const int tw = renderer.getTextWidth(PULSR_10_FONT_ID, abbr);
    const int th = renderer.getTextHeight(PULSR_10_FONT_ID);
    const int ty = (barH - th) / 2;

    if (tabs[i].selected) {
      // White filled pill with black text
      constexpr int PAD_X = 6, PAD_Y = 5;
      renderer.fillRect(tx + 2, PAD_Y, tabW - 4, barH - PAD_Y * 2, /*black=*/false);
      renderer.drawText(PULSR_10_FONT_ID, tx + (tabW - tw) / 2, ty, abbr, /*black=*/true);
    } else {
      // White text on black bar
      renderer.drawText(PULSR_10_FONT_ID, tx + (tabW - tw) / 2, ty, abbr, /*black=*/false);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawList  –  Paged list of items with optional subtitle / value / icon.
// Selected row: light-gray fill + left-edge indicator bar.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount,
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

    // Selection highlight: light-gray row fill flush to content edge
    // (no separate left indicator bar — the PULSR left bar already provides that anchor)
    if (isSel) {
      renderer.fillRectDither(cr.x, itemY, contentW, rowH, Color::LightGray);
    }

    // Row separator (thin black line at bottom of row)
    renderer.drawLine(cr.x + SEL_BAR_W + LIST_PAD_X, itemY + rowH - 1,
                      cr.x + contentW - 1, itemY + rowH - 1);

    // Value text (right-aligned), computed before title to know available width
    int valueW = 0;
    std::string valueStr;
    if (rowValue != nullptr) {
      valueStr = rowValue(i);
      valueStr = renderer.truncatedText(PULSR_10_FONT_ID, valueStr.c_str(), maxValueW);
      valueW   = renderer.getTextWidth(PULSR_10_FONT_ID, valueStr.c_str()) + LIST_PAD_X;
    }

    // Title text
    const int titleX  = cr.x + SEL_BAR_W + LIST_PAD_X;
    const int titleMaxW = contentW - SEL_BAR_W - LIST_PAD_X * 2 - valueW;
    const int titleH  = renderer.getTextHeight(PULSR_10_FONT_ID);
    const int titleY  = rowSubtitle != nullptr ? itemY + 5
                                               : itemY + (rowH - titleH) / 2;

    const auto titleTrunc = renderer.truncatedText(PULSR_10_FONT_ID, rowTitle(i).c_str(), titleMaxW);
    renderer.drawText(PULSR_10_FONT_ID, titleX, titleY, titleTrunc.c_str(), /*black=*/true);

    // Subtitle text
    if (rowSubtitle != nullptr) {
      const auto sub = renderer.truncatedText(SMALL_FONT_ID, rowSubtitle(i).c_str(), titleMaxW);
      renderer.drawText(SMALL_FONT_ID, titleX, itemY + titleH + 8, sub.c_str(), /*black=*/true);
    }

    // Value text (draw right-aligned)
    if (!valueStr.empty()) {
      const int vx = cr.x + contentW - renderer.getTextWidth(PULSR_10_FONT_ID, valueStr.c_str()) -
                     LIST_PAD_X;
      const int vy = itemY + (rowH - renderer.getTextHeight(PULSR_10_FONT_ID)) / 2;
      if (isSel && highlightValue) {
        const int boxW = renderer.getTextWidth(PULSR_10_FONT_ID, valueStr.c_str()) + LIST_PAD_X * 2;
        renderer.fillRect(vx - LIST_PAD_X, itemY, boxW, rowH, /*black=*/true);
        renderer.drawText(PULSR_10_FONT_ID, vx, vy, valueStr.c_str(), /*black=*/false);
      } else {
        renderer.drawText(PULSR_10_FONT_ID, vx, vy, valueStr.c_str(), /*black=*/true);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawButtonHints  –  Action button labels in the bottom bar.
// The bottom bar is already filled black by drawFrame.  We draw white-outlined
// boxes for non-empty labels and white text inside them.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2,
                                  const char* btn3, const char* btn4) const {
  const GfxRenderer::Orientation origOri = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int W     = renderer.getScreenWidth();
  const int H     = renderer.getScreenHeight();
  const int barY  = H - TOP_H;
  const int barH  = TOP_H;
  const int avail = W - LEFT_W;
  const int btnW  = avail / BTN_COUNT;  // no gaps – flush dividers only

  const char* labels[BTN_COUNT] = {btn1, btn2, btn3, btn4};

  // White vertical separator lines between button zones
  for (int i = 1; i < BTN_COUNT; i++) {
    const int lx = LEFT_W + i * btnW;
    renderer.drawLine(lx, barY, lx, barY + barH - 1, /*black=*/false);
  }

  // White uppercase label centered in each zone
  for (int i = 0; i < BTN_COUNT; i++) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;

    // Uppercase copy
    char upper[32];
    int c = 0;
    for (; labels[i][c] && c < 31; c++) {
      upper[c] = (labels[i][c] >= 'a' && labels[i][c] <= 'z') ? labels[i][c] - 32 : labels[i][c];
    }
    upper[c] = '\0';

    const int x  = LEFT_W + i * btnW;
    const int tw = renderer.getTextWidth(PULSR_10_FONT_ID, upper);
    const int th = renderer.getTextHeight(PULSR_10_FONT_ID);
    const int tx = x + (btnW - tw) / 2;
    const int ty = barY + (barH - th) / 2;
    renderer.drawText(PULSR_10_FONT_ID, tx, ty, upper, /*black=*/false);
  }

  renderer.setOrientation(origOri);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawSideButtonHints  –  Not visible in PULSR (left bar serves this role).
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawSideButtonHints(const GfxRenderer& /*renderer*/, const char* /*topBtn*/,
                                      const char* /*bottomBtn*/) const {
  // Intentionally empty: PULSR left bar provides navigation context visually.
}

// ─────────────────────────────────────────────────────────────────────────────
// drawButtonMenu  –  Home screen menu tiles in a 2-column grid.
// Selected tile: light-gray fill + black border + black text (consistent with list selection).
// Others: outlined box, black text.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawButtonMenu(GfxRenderer& renderer, Rect /*rect*/, int buttonCount,
                                 int selectedIndex,
                                 const std::function<std::string(int)>& buttonLabel,
                                 const std::function<UIIcon(int)>& /*rowIcon*/) const {
  // 2-column pill grid, bottom-pinned just above the bottom bar.
  const int W        = renderer.getScreenWidth();
  const int H        = renderer.getScreenHeight();
  constexpr int COLS    = 2;
  constexpr int HGAP    = 8;   // horizontal gap between pills
  constexpr int VGAP    = 6;   // vertical gap between rows
  constexpr int BTN_PAD = 8;   // inner horizontal margin for button grid (independent of contentSidePadding)
  const int tileH    = Lm::values.menuRowHeight;
  const int tileW    = (W - LEFT_W - BTN_PAD * 2 - HGAP) / COLS;
  const int rows     = (buttonCount + COLS - 1) / COLS;
  const int gridH    = rows * tileH + (rows - 1) * VGAP;
  const int startY   = H - TOP_H - gridH - 8;
  const int radius   = tileH / 2;  // fully rounded ends = pill shape

  // PULSR label abbreviation: uppercase and strip generic qualifier words
  auto pulsrLabel = [](const std::string& s) -> std::string {
    std::string u = s;
    for (char& c : u) c = (c >= 'a' && c <= 'z') ? c - 32 : c;
    // Strip leading generic words
    auto stripPrefix = [&](const char* pfx) {
      size_t pl = strlen(pfx);
      if (u.size() > pl && u.substr(0, pl) == pfx) u = u.substr(pl);
    };
    // Strip trailing generic qualifiers
    auto stripSuffix = [&](const char* sfx) {
      size_t sl = strlen(sfx);
      if (u.size() > sl && u.substr(u.size() - sl) == sfx) u = u.substr(0, u.size() - sl);
    };
    stripPrefix("FILE ");
    stripPrefix("MY ");
    stripSuffix(" FILES");
    stripSuffix(" BOOKS");
    return u;
  };

  for (int i = 0; i < buttonCount; i++) {
    const int col  = i % COLS;
    const int row  = i / COLS;
    const int tx   = LEFT_W + BTN_PAD + col * (tileW + HGAP);
    const int ty   = startY + row * (tileH + VGAP);
    const bool sel = (i == selectedIndex);

    if (sel) {
      // Selected: light-gray pill, black outline, black text
      renderer.fillRoundedRect(tx, ty, tileW, tileH, radius, Color::LightGray);
    }
    // Always draw outline (selected and unselected)
    renderer.drawRoundedRect(tx, ty, tileW, tileH, /*lineWidth=*/1, radius, /*black=*/true);

    const std::string labelStr = pulsrLabel(buttonLabel(i));
    const int lw = renderer.getTextWidth(PULSR_10_FONT_ID, labelStr.c_str());
    const int lh = renderer.getTextHeight(PULSR_10_FONT_ID);
    renderer.drawText(PULSR_10_FONT_ID, tx + (tileW - lw) / 2, ty + (tileH - lh) / 2,
                      labelStr.c_str(), /*black=*/true);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawRecentBookCover  –  Home screen recent books panel.
// Horizontal row layout: cover thumbnail on left, title + author on right.
// Covers are loaded from SD on the first render and saved to the frame buffer
// via storeCoverBuffer so subsequent renders only redraw selection / text.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                      const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered,
                                      bool& coverBufferStored, bool& /*bufferRestored*/,
                                      std::function<bool()> storeCoverBuffer) const {
  const Rect cr      = contentRect(rect);
  const int maxBooks = Lm::values.homeRecentBooksCount;
  const int count    = std::min(static_cast<int>(recentBooks.size()), maxBooks);
  const int rowH     = cr.height / maxBooks;

  if (count == 0) {
    const int msgY = cr.y + cr.height / 2 - renderer.getTextHeight(PULSR_10_FONT_ID);
    renderer.drawCenteredText(PULSR_10_FONT_ID, msgY, tr(STR_NO_OPEN_BOOK), /*black=*/true);
    renderer.drawCenteredText(PULSR_10_FONT_ID, msgY + renderer.getTextHeight(PULSR_10_FONT_ID) + 4,
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
        if (Storage.openFileForRead("PULSR_HOME", bmpPath, file)) {
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
      // Fill the full row width with grey — the PULSR left bar is already black
      // so no separate selection indicator is needed and the left edge stays flush.
      renderer.fillRectDither(cr.x, rowY, cr.width, topInset, Color::LightGray);
      renderer.fillRectDither(cr.x, coverEndY, cr.width, topInset, Color::LightGray);
      renderer.fillRectDither(cr.x, coverStartY, thumbX - cr.x, coverH, Color::LightGray);
      renderer.fillRectDither(thumbEndX, rowY, cr.x + cr.width - thumbEndX, rowH, Color::LightGray);
    }


    // Title + author in the text column (right of cover)
    const int textX    = thumbEndX + LIST_PAD_X;
    const int textMaxW = cr.x + cr.width - textX - LIST_PAD_X;
    const int titleH   = renderer.getTextHeight(PULSR_10_FONT_ID);
    const int authH    = renderer.getTextHeight(SMALL_FONT_ID);
    const int textY    = rowY + (rowH - titleH - 4 - authH) / 2;

    const auto titleTrunc =
        renderer.truncatedText(PULSR_10_FONT_ID, book.title.c_str(), textMaxW, EpdFontFamily::BOLD);
    renderer.drawText(PULSR_10_FONT_ID, textX, textY, titleTrunc.c_str(), /*black=*/true,
                      EpdFontFamily::BOLD);

    if (!book.author.empty()) {
      const auto authTrunc = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), textMaxW);
      renderer.drawText(SMALL_FONT_ID, textX, textY + titleH + 4, authTrunc.c_str(), /*black=*/true);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawProgressBar  –  PULSR-style segmented progress bar (e.g. downloads).
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current,
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
  renderer.drawCenteredText(PULSR_10_FONT_ID, cr.y + cr.height + 10, pctBuf);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawReadingProgressBar  –  Thin progress strip along the top of the content
// area (visible above the book text, below the black top bar).
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawReadingProgressBar(const GfxRenderer& renderer,
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
Rect PulsrTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const int W       = renderer.getScreenWidth();
  const int textW   = renderer.getTextWidth(PULSR_12_FONT_ID, message);
  const int textH   = renderer.getLineHeight(PULSR_12_FONT_ID);
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
  renderer.drawText(PULSR_12_FONT_ID, tx, ty, message, /*black=*/false);
  renderer.displayBuffer();

  return Rect{boxX, boxY, boxW, boxH};
}

// ─────────────────────────────────────────────────────────────────────────────
// fillPopupProgress  –  Fills an expanding progress line inside a popup.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout,
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
void PulsrTheme::drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth) const {
  constexpr int PAD    = 8;
  const int lineW      = textWidth + PAD * 2;
  const int lineY      = rect.y + rect.height + renderer.getLineHeight(PULSR_12_FONT_ID) +
                         Lm::values.verticalSpacing;
  const int lineX      = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineX, lineY, lineX + lineW, lineY, /*lineWidth=*/2, /*black=*/true);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawKeyboardKey  –  Single keyboard key, filled when selected.
// ─────────────────────────────────────────────────────────────────────────────
void PulsrTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label,
                                  bool isSelected) const {
  constexpr int R = 3;
  if (isSelected) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, R, Color::Black);
  } else {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, R, /*black=*/true);
  }
  const int tw = renderer.getTextWidth(PULSR_12_FONT_ID, label);
  const int th = renderer.getTextHeight(PULSR_12_FONT_ID);
  renderer.drawText(PULSR_12_FONT_ID, rect.x + (rect.width - tw) / 2,
                    rect.y + (rect.height - th) / 2, label, /*black=*/!isSelected);
}
