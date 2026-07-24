#pragma once

#include "components/themes/lyra/LyraTheme.h"

struct HomeBookSummary;

namespace CrossViMetrics {
constexpr int HOME_READING_SUMMARY_HEIGHT = 68;
constexpr int HOME_READING_SUMMARY_GAP = 8;
constexpr int HOME_READING_SUMMARY_BOTTOM_GAP = 24;
constexpr ThemeMetrics makeValues() {
  ThemeMetrics values = LyraMetrics::values;
  values.homeTopPadding = 56;
  values.homeCoverHeight = 168;
  values.homeCoverTileHeight = 252;
  values.homeRecentBooksCount = 1;
  values.homeContinueReadingInMenu = false;
  values.homeMenuTopOffset = 12;
  values.menuRowHeight = 80;
  values.menuSpacing = 10;
  return values;
}
constexpr ThemeMetrics values = makeValues();
}  // namespace CrossViMetrics

class CrossViTheme final : public LyraTheme {
 public:
  void drawHomeHeader(const GfxRenderer& renderer, Rect rect, const char* title) const override;
  Rect getHomeCoverCacheRect(Rect tileRect) const override;
  void drawHomeContent(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                       bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                       std::function<bool()> storeCoverBuffer, const HomeBookSummary& summary) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawHomeReadingSummary(const GfxRenderer& renderer, Rect rect, const HomeBookSummary& summary,
                              bool selected) const override;
};
