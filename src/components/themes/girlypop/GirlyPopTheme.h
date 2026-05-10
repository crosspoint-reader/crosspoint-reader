#pragma once

#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace GirlyPopMetrics {
// Slightly taller header to fit the double-frame border with breathing room.
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 0,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 55,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 42,
                                 .listWithSubtitleRowHeight = 69,
                                 .menuRowHeight = 42,
                                 .menuSpacing = 6,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 65,
                                 .homeCoverHeight = 300,
                                 .homeCoverTileHeight = 350,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = true,
                                 .homeMenuTopOffset = 20,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 30,
                                 .keyboardKeySpacing = 10,
                                 .keyboardBottomKeyHeight = 30,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = 0,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 12};
}

class GirlyPopTheme : public RoundedRaffTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
};
