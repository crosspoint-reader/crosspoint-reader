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
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 242,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeShowContinueReadingHeader = true,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 6,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = false,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 6,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class LyraTheme : public BaseTheme {
 public:
  explicit LyraTheme(const ThemeMetrics* metrics = &LyraMetrics::values,
                     const ThemeHomeRecentsSpec* homeRecents = nullptr, const ThemeButtonMenuSpec* buttonMenu = nullptr,
                     const ThemeListSpec* list = nullptr, const ThemeButtonHintsSpec* buttonHints = nullptr,
                     const ThemeTabBarSpec* tabBar = nullptr, const ThemeHeaderSpec* header = nullptr,
                     const char* assetRoot = nullptr, const ThemeIconMap* icons = nullptr)
      : metrics_(metrics),
        homeRecents_(homeRecents),
        buttonMenu_(buttonMenu),
        list_(list),
        buttonHints_(buttonHints),
        tabBar_(tabBar),
        header_(header),
        assetRoot_(assetRoot),
        icons_(icons) {}

  // Component drawing methods
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  bool homeCoverCacheDependsOnSelector() const override {
    return homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::CoverStrip;
  }
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer, bool coverStripSelected = true) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  bool showsFileIcons() const override { return true; }

 private:
  const ThemeMetrics* metrics_;
  const ThemeHomeRecentsSpec* homeRecents_;
  const ThemeButtonMenuSpec* buttonMenu_;
  const ThemeListSpec* list_;
  const ThemeButtonHintsSpec* buttonHints_;
  const ThemeTabBarSpec* tabBar_;
  const ThemeHeaderSpec* header_;
  const char* assetRoot_;
  const ThemeIconMap* icons_;
  const ThemeMetrics& metrics() const { return metrics_ ? *metrics_ : LyraMetrics::values; }
  bool hasThemeIcon(UIIcon icon) const;
  bool drawThemeIcon(const GfxRenderer& renderer, UIIcon icon, int x, int y, int size) const;
  void drawCoverStripRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                             int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool bufferRestored,
                             std::function<bool()> storeCoverBuffer, bool coverStripSelected) const;
};
