#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace NoirMetrics {
constexpr ThemeMetrics values = {
    .batteryWidth = 16,
    .batteryHeight = 12,
    .topPadding = 5,
    .batteryBarHeight = 40,
    .headerHeight = 72,
    .verticalSpacing = 12,
    .contentSidePadding = 20,
    .listRowHeight = 40,
    .listWithSubtitleRowHeight = 60,
    .menuRowHeight = 56,
    .menuSpacing = 6,
    .tabSpacing = 8,
    .tabBarHeight = 40,
    .scrollBarWidth = 4,
    .scrollBarRightOffset = 5,
    .homeTopPadding = 50,
    .homeCoverHeight = 226,
    .homeCoverTileHeight = 242,
    .homeRecentBooksCount = 1,
    .buttonHintsHeight = 40,
    .sideButtonHintsWidth = 30,
    .progressBarHeight = 16,
    .progressBarMarginTop = 1,
    .statusBarHorizontalMargin = 5,
    .statusBarVerticalMargin = 19,
    .keyboardKeyWidth = 31,
    .keyboardKeyHeight = 50,
    .keyboardKeySpacing = 0,
    .keyboardBottomAligned = true,
    .keyboardCenteredText = true
};
}

class NoirTheme : public LyraTheme {
 public:
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon,
                const std::function<std::string(int index)>& rowValue,
                bool highlightValue) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
};
