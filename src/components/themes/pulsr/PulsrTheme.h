#pragma once

#include <GfxRenderer.h>

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Layout (portrait, 480×800 device pixels):
//   - Left bar:   x 0..88   (full height, rounded left corners)
//   - Top bar:    y 0..44   (full width)
//   - Bottom bar: y 756..800 (full width)
//   - Content:    x 88..480, y 44..756 (white area)
//
// Inner elbow curves sit at the junctions of the left bar with the top/bottom bars.

namespace PulsrMetrics {
// Frame geometry constants
constexpr int LEFT_BAR_W = 88;    // Left bar width (px)
constexpr int TOP_BAR_H = 44;     // Top and bottom bar height (px)
constexpr int OUTER_RADIUS = 56;  // Outer left-corner curve radius (px)
constexpr int NAV_GAP = 56;       // Nav zone margin matching outer radius (px)

// clang-format off
constexpr ThemeMetrics values = {
  .batteryWidth              = 15,
  .batteryHeight             = 12,
  .topPadding                = 0,
  .batteryBarHeight          = TOP_BAR_H,
  .headerHeight              = TOP_BAR_H,
  .verticalSpacing           = 8,
  .contentSidePadding        = 98,  // LEFT_BAR_W(88) + 10 — pushes content past PULSR left bar
  .listRowHeight             = 36,
  .listWithSubtitleRowHeight = 56,
  .menuRowHeight             = 44,
  .menuSpacing               = 0,
  .tabSpacing                = 0,
  .tabBarHeight              = 0,   // tabs live inside the top bar
  .scrollBarWidth            = 4,
  .scrollBarRightOffset      = 5,
  .homeTopPadding            = TOP_BAR_H,
  .homeCoverHeight           = 185,
  .homeCoverTileHeight       = 610,
  .homeRecentBooksCount      = 3,
  .buttonHintsHeight         = TOP_BAR_H,
  .sideButtonHintsWidth      = 0,
  .progressBarHeight         = 14,

  .keyboardKeyWidth          = 24,  // narrower keys center keyboard at x=100, past 88px left bar
  .keyboardKeyHeight         = 42,
  .keyboardKeySpacing        = 4,
  .keyboardBottomAligned     = true,
  .keyboardCenteredText      = true,
};
// clang-format on
}  // namespace PulsrMetrics

class PulsrTheme : public BaseTheme {
 public:
  PulsrTheme() : inverted_(false) {}

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
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const override;
  void drawReadingProgressBar(const GfxRenderer& renderer, size_t bookProgress) const override;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, int progress) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth) const override;
  void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, bool isSelected) const override;
  // PULSR chrome owns the full frame; these shared helpers are no-ops or
  // PULSR-styled to avoid bleeding Classic/Lyra elements onto PULSR screens.
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage) const override {}
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage) const override {}
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0) const override {}
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const override;

 protected:
  explicit PulsrTheme(bool inverted) : inverted_(inverted) {}

  bool isInverted() const override { return inverted_; }

  // Color helpers — flip black/white for inverted (dark) theme.
  bool b(bool black) const { return inverted_ ? !black : black; }
  Color col(Color c) const {
    if (!inverted_) return c;
    switch (c) {
      case Color::Black:     return Color::White;
      case Color::White:     return Color::Black;
      case Color::LightGray: return Color::DarkGray;
      case Color::DarkGray:  return Color::LightGray;
      default:               return c;
    }
  }

 private:
  bool inverted_;

  // Draws the full PULSR chrome: left bar, top bar, bottom bar, inner elbow curves, title and
  // battery text in the top bar, and nav segment decorators in the left bar.
  void drawFrame(const GfxRenderer& renderer, const char* title) const;
};
