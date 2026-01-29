#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "components/UITheme.h"

class GfxRenderer;
struct RecentBookInfo;

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 20,
                                 .homeCoverHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .versionTextRightX = 20,
                                 .versionTextY = 738,
                                 .bookProgressBarHeight = 4};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  virtual void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  virtual void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true);
  virtual void drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4);
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn);
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                        const std::function<std::string(int index)>& rowIcon, bool hasValue,
                        const std::function<std::string(int index)>& rowValue);

  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title);
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs);
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                   const std::vector<RecentBookWithCover>& recentBooks, const int selectorIndex,
                                   bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                   std::function<bool()> storeCoverBuffer);
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel, bool hasIcon,
                              const std::function<std::string(int index)>& rowIcon);
  virtual void drawPopup(GfxRenderer& renderer, const char* message);
  virtual PopupCallbacks drawPopupWithProgress(GfxRenderer& renderer, const std::string& title);
  virtual void drawBookProgressBar(const GfxRenderer& renderer, const size_t bookProgress);
};