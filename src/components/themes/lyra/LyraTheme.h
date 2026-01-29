#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 40,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 20,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 266,
                                 .homeRecentBooksCount = 3,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 19,
                                 .versionTextRightX = 20,
                                 .versionTextY = 55,
                                 .bookProgressBarHeight = 4};
}

class LyraTheme : public BaseTheme {
 public:
  // Component drawing methods
  //   void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) override;
  void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs) override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                const std::function<std::string(int index)>& rowIcon, bool hasValue,
                const std::function<std::string(int index)>& rowValue) override;
  void drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel, bool hasIcon,
                      const std::function<std::string(int index)>& rowIcon) override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBookWithCover>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) override;
  void drawPopup(GfxRenderer& renderer, const char* message) override;
};
