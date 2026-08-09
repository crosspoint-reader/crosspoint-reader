#pragma once

#include <functional>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

class GfxRenderer;
struct RecentBook;

namespace NewspaperMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = BaseMetrics::values;
  v.topPadding = 6;
  v.contentSidePadding = 18;
  v.homeTopPadding = 96;        // masthead + double rule + count/battery band + hairline
  v.homeCoverHeight = 150;      // thumbnail cache height for the lead block
  v.homeCoverTileHeight = 186;  // lead block + heavy closing rule
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = true;
  v.homeMenuTopOffset = 8;
  v.menuRowHeight = 38;
  v.menuSpacing = 0;
  v.buttonHintsHeight = 26;
  return v;
}();
}  // namespace NewspaperMetrics

// Theme E -- Newspaper.
//
// Serif masthead, double rule, a band carrying the recent-book count and battery, a
// lead block with the cover set as a halftone beside a serif headline, a heavy
// closing rule, then hairline-separated menu rows with a full-bleed inverted bar for
// the selection.
//
// FONTS: the masthead, headline and author use NOTOSERIF_18/16. Both are registered
// in main.cpp behind `#ifndef OMIT_FONTS`, which is not defined by any env in
// platformio.ini. NOTOSERIF_14 is the only unconditional serif -- if OMIT_FONTS is
// ever introduced, fall back to that. The menu rows stay in the regular UI font so
// they match the other themes.
//
// HALFTONE: cover thumbnails are already 1-bit dithered BMPs, so drawing one gives
// the halftone look at no cost. The no-cover placeholder uses fillRectDither, which
// is the public dithering primitive (drawPixelDither is private to GfxRenderer).
class NewspaperTheme : public BaseTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
};
