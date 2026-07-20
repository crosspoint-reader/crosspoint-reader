#pragma once

#include "components/themes/lyra/LyraTheme.h"

struct HomeBookSummary;

namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics values = LyraMetrics::values;
  values.homeCoverHeight = 252;
  values.homeCoverTileHeight = 410;
  values.homeRecentBooksCount = 1;
  values.homeContinueReadingInMenu = false;
  values.homeMenuTopOffset = 8;
  values.menuRowHeight = 48;
  values.menuSpacing = 8;
  return values;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace DashboardMetrics

class DashboardTheme final : public LyraTheme {
 public:
  Rect getHomeCoverCacheRect(Rect tileRect) const override;
  void drawHomeContent(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                       bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                       std::function<bool()> storeCoverBuffer, const HomeBookSummary& summary) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
};
