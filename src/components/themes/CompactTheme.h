
#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Compact theme metrics (zero runtime cost)
namespace CompactMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 20,
                                 .batteryHeight = 12,
                                 .topPadding = 2,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 28,
                                 .verticalSpacing = 16,
                                 .contentSidePadding = 4,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 48,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 28,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 276,
                                 .homeRecentBooksCount = 3,
                                 .buttonHintsHeight = 30,
                                 .sideButtonHintsWidth = 28,
                                 .progressBarHeight = 16,
                                 .bookProgressBarHeight = 4,
                                 .keyboardKeyWidth = 30,
                                 .keyboardKeyHeight = 30,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = true};
}

class CompactTheme : public BaseTheme {
 public:
  // Component drawing methods
  //   void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) override;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth) const override;
  void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected) const override;
 private:
  void drawBatteryIcon(const GfxRenderer& renderer, Rect rect, const uint16_t percentage) const;
  void drawBatteryText(const GfxRenderer& renderer, const int x, const int y, const uint16_t percentage, const bool right = false) const;
};
