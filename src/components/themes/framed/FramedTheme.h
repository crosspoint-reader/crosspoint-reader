#pragma once

#include <functional>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

class GfxRenderer;
struct RecentBook;

namespace FramedMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = BaseMetrics::values;
  // Everything sits inside an 8px hairline frame, so content is pushed in further
  // than stock to keep it off the border.
  v.topPadding = 14;
  v.contentSidePadding = 26;
  v.homeTopPadding = 44;        // status bar band, inside the frame
  v.homeCoverHeight = 240;      // thumbnail cache height (regenerated on first use)
  v.homeCoverTileHeight = 320;  // cover + title + author + rule
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = true;
  v.homeMenuTopOffset = 14;
  v.menuRowHeight = 32;
  v.menuSpacing = 4;
  v.buttonHintsHeight = 26;  // flat footer, 14px reclaimed from stock's 40
  return v;
}();
}  // namespace FramedMetrics

// Theme B -- Framed.
//
// A single hairline frame inset 8px on all sides, stopping above the footer. One
// centred cover with the title and author centred beneath it, a rule, then plain
// menu rows. The selected row is marked with a 4px filled left tab and a bold label
// rather than an inverted bar, which keeps e-ink repaint area small.
class FramedTheme : public BaseTheme {
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
