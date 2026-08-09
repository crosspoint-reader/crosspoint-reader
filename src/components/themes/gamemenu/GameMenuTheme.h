#pragma once

#include <functional>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "components/themes/shared/ThemeShared.h"

class GfxRenderer;
struct RecentBook;

namespace GameMenuMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = BaseMetrics::values;
  v.topPadding = 0;
  v.contentSidePadding = 22;
  v.homeTopPadding = 38;        // status bar band
  v.homeCoverHeight = 130;      // thumbnail cache height, sized for the hero card
  v.homeCoverTileHeight = 172;  // hero card + gap
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = true;
  v.homeMenuTopOffset = 16;
  v.menuRowHeight = 62;
  v.menuSpacing = 12;
  v.buttonHintsHeight = ThemeShared::kFooterBandHeight;  // 26px visible + 10px padding below
  return v;
}();
}  // namespace GameMenuMetrics

// Theme C -- Game menu.
//
// A large hero card carrying the cover, the Continue reading label, the title and the
// author, with the remaining menu entries as rounded cards beneath it. One selection
// rule throughout: selected fills black, unselected stays outlined. The hero is
// simply the biggest card, and follows the same rule.
//
// COORDINATION NOTE: "Continue reading" is menu index 0 (HomeActivity inserts it when
// homeContinueReadingInMenu is set and there is at least one recent book). The hero
// card already renders that entry, so drawButtonMenu must skip index 0 or it would be
// drawn twice. drawRecentBookCover runs first in HomeActivity::render, so it records
// whether a hero was drawn in `heroActive_` and drawButtonMenu reads it back. Render
// is single-threaded, so this ordering is safe.
//
// E-INK NOTE: moving the selection on or off the hero repaints a large mostly-black
// region. This theme will flash more than the other three.
class GameMenuTheme : public BaseTheme {
  mutable bool heroActive_ = false;

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
