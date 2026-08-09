#pragma once

class BaseTheme;
class GfxRenderer;
struct Rect;

namespace ThemeShared {

// Flat footer shared by the Compact, Framed, Game Menu and Newspaper themes.
//
// Stock `BaseTheme::drawButtonHints` draws an outlined 106px box per label. These
// themes keep the stock slot geometry (so each label still sits above its physical
// button) but drop the outline, leaving a hairline rule across the top of the band.
//
// The white clearing fill is deliberately kept: without it, partial refresh leaves
// the previous labels smeared underneath the new ones.
//
// `footerHeight` must be the calling theme's own `buttonHintsHeight`. Do not read it
// from BaseMetrics -- HomeActivity reserves space using the *active* theme's value,
// so a mismatch offsets the whole footer.
void drawFlatButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                         int footerHeight);

// Status bar shared by all four themes: book title on the left (ellipsised), battery
// icon and percentage on the right. `theme` is only needed to reach the inherited
// `drawBatteryRight`, which is a non-static BaseTheme member.
void drawTitleStatusBar(const BaseTheme& theme, const GfxRenderer& renderer, Rect rect, const char* title,
                        int sidePadding, int batteryWidth, int batteryHeight, int titleFontId);

}  // namespace ThemeShared
