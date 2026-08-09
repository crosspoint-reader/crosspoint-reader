#pragma once

class BaseTheme;
class GfxRenderer;
struct Rect;

namespace ThemeShared {

// --- Footer geometry -------------------------------------------------------
//
// The footer is a band reserved at the bottom of the screen. HomeActivity sizes
// the menu rect by subtracting the active theme's `buttonHintsHeight`, so that
// metric must equal the WHOLE band -- visible part plus the blank padding below
// it -- or the menu will overlap the labels.
//
//   kFooterVisualHeight   hairline rule + label text
//   kFooterBottomPadding  blank space beneath, lifting the footer off the edge
//   kFooterBandHeight     what a theme puts in `buttonHintsHeight`
//
// Raise kFooterBottomPadding to float the footer higher; nothing else needs
// touching, because every theme derives buttonHintsHeight from these.
constexpr int kFooterVisualHeight = 26;
constexpr int kFooterBottomPadding = 10;
constexpr int kFooterBandHeight = kFooterVisualHeight + kFooterBottomPadding;

// Flat footer shared by the Compact, Framed, Game Menu and Newspaper themes.
//
// Stock `BaseTheme::drawButtonHints` draws an outlined 106px box per label. These
// themes keep the stock slot geometry (so each label still sits above its physical
// button) but drop the outline, leaving a hairline rule across the top.
//
// The white clearing fill covers the full band including the padding: without it,
// partial refresh leaves the previous labels smeared underneath the new ones.
void drawFlatButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                         int bandHeight = kFooterBandHeight, int bottomPadding = kFooterBottomPadding);

// Status bar shared by all four themes: book title on the left (ellipsised), battery
// icon and percentage on the right. `theme` is only needed to reach the inherited
// `drawBatteryRight`, which is a non-static BaseTheme member.
void drawTitleStatusBar(const BaseTheme& theme, const GfxRenderer& renderer, Rect rect, const char* title,
                        int sidePadding, int batteryWidth, int batteryHeight, int titleFontId);

}  // namespace ThemeShared
