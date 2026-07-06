#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

namespace RoundedRaffMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 0,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 42,
                                 .listWithSubtitleRowHeight = 69,
                                 .menuRowHeight = 42,
                                 .menuSpacing = 6,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 55,
                                 // Smaller cover tile so the home menu sits higher (fits 5 items without overlap).
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
                                 .keyboardKeyCornerRadius = 10,
                                 .keyboardFillUnselected = true,
                                 .keyboardOutlineAllUnselected = true,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 3,
                                 .keyboardSecondaryLabelTopPadding = 1,
                                 .keyboardMinArrowHeadSize = 1,
                                 .popupTopOffsetRatio = 0.12f,
                                 .popupMarginX = 20,
                                 .popupMarginY = 14,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 18,
                                 .popupTextBold = true,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = true,
                                 .popupProgressClampPercent = true,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .textFieldHorizontalPadding = 8,
                                 .textFieldNormalThickness = 2,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = -1};
}

class RoundedRaffTheme : public BaseTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const override;
  void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, bool isSelected,
                       const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                       bool inactiveSelection = false) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = nullptr,
                const std::function<UIIcon(int index)>& rowIcon = nullptr,
                const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  bool homeMenuShowsContinueReading() const { return true; }
};
