#pragma once

#include <functional>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "components/themes/shared/ThemeShared.h"

class GfxRenderer;
struct RecentBook;

namespace CompactMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = BaseMetrics::values;
  v.topPadding = 0;
  v.contentSidePadding = 14;
  v.homeTopPadding = 34;       // status bar band
  v.homeCoverHeight = 96;      // thumbnail cache height; the strip draws it at ~34x48
  v.homeCoverTileHeight = 72;  // horizontal now-reading strip + rule
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = true;
  v.homeMenuTopOffset = 6;
  v.menuRowHeight = 26;
  v.menuSpacing = 0;
  v.buttonHintsHeight = ThemeShared::kFooterBandHeight;  // 26px visible + 10px padding below
  return v;
}();
}  // namespace CompactMetrics

// Theme A -- Compact.
//
// A horizontal now-reading strip (small cover left, title and author stacked right),
// a rule, then tight 26px menu rows each closed by a hairline divider. The selected
// row is a full-bleed inverted bar.
//
// NOTE ON VERTICAL FILL: six 26px rows occupy ~156px starting around y=112, on a
// 792-800px screen. That leaves a large empty band above the footer. This is what
// "compact" specifies, but if you would rather the rows spread to fill the space,
// raise `menuRowHeight` to 26 -> 74 (or set `kFillMenuHeight` below to true).
class CompactTheme : public BaseTheme {
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
