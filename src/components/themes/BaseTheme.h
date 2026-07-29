#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionHPadding;
  int optionPopupSelectionVPadding;
  int optionPopupTitleGap;
  bool optionPopupUseSmallFont;
  bool optionPopupOptionFontBold;
  int optionPopupSelectionRadius;
  bool optionPopupSelectionLight;
  bool optionPopupDrawAllRows;
  int optionPopupDialogSideMargin;
  bool optionPopupTitleSeparator;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;
};

enum UIIcon { None = 0, Folder, Text, Image, Book, File, Recent, Settings, Transfer, Library, Wifi, Hotspot, Bookmark, Music };

// Small-panel density scale. The metric sets below were hand-tuned for the
// 480x800-logical X4/X3 class; the Murphy M3's logical canvas is 240x416 at
// ~130 PPI, so the same pixel values render both physically larger and out of
// proportion to the halved canvas. Each theme's constexpr set (not the
// getMetrics() accessor — theme drawing reads the sets directly) is passed
// through scaledMetrics() at compile time. Touch-relevant heights are floored
// so the scale can't produce sub-finger targets (~28px = 5.5mm at 130 PPI).
#if FREEINK_DEVICE_MURPHY
constexpr float kUiDensityScale = 0.6f;
#else
constexpr float kUiDensityScale = 1.0f;
#endif

constexpr int scaledDim(int v) { return static_cast<int>(v * kUiDensityScale + 0.5f); }
constexpr int scaledTouchDim(int v) {
  return (v >= 28 && scaledDim(v) < 28) ? 28 : scaledDim(v);
}

constexpr ThemeMetrics scaledMetrics(ThemeMetrics m) {
  m.batteryWidth = scaledDim(m.batteryWidth);
  m.batteryHeight = scaledDim(m.batteryHeight);
  m.topPadding = scaledDim(m.topPadding);
  m.batteryBarHeight = scaledDim(m.batteryBarHeight);
  m.headerHeight = scaledDim(m.headerHeight);
  m.verticalSpacing = scaledDim(m.verticalSpacing);
  m.previewPadding = scaledDim(m.previewPadding);
  m.contentSidePadding = scaledDim(m.contentSidePadding);
  m.listRowHeight = scaledTouchDim(m.listRowHeight);
  m.listWithSubtitleRowHeight = scaledTouchDim(m.listWithSubtitleRowHeight);
  m.menuRowHeight = scaledTouchDim(m.menuRowHeight);
  m.menuSpacing = scaledDim(m.menuSpacing);
  m.tabSpacing = scaledDim(m.tabSpacing);
  m.tabBarHeight = scaledTouchDim(m.tabBarHeight);
  m.scrollBarRightOffset = scaledDim(m.scrollBarRightOffset);
  m.homeTopPadding = scaledDim(m.homeTopPadding);
  m.homeCoverHeight = scaledDim(m.homeCoverHeight);
  m.homeCoverTileHeight = scaledDim(m.homeCoverTileHeight);
#if FREEINK_DEVICE_MURPHY
  // Plain 0.6x keeps the X4's cover tile at 58% of the murphy's 416px screen,
  // which leaves room for only 4 menu rows below it (5 with the Audio entry
  // overflow the panel). Cap the tile so 6 rows (OPDS + Audio both enabled)
  // fit: 24 top pad + 186 tile + 6 offset + 6 spacing + 6*(28+5) = 420 ~ 416
  // with the last row's trailing gap clipped, all rows fully visible.
  if (m.homeCoverTileHeight > 186) m.homeCoverTileHeight = 186;
  if (m.homeCoverHeight > 186) m.homeCoverHeight = 186;
#endif
  m.homeMenuTopOffset = scaledDim(m.homeMenuTopOffset);
  m.buttonHintsHeight = scaledDim(m.buttonHintsHeight);
  m.sideButtonHintsWidth = scaledDim(m.sideButtonHintsWidth);
  m.progressBarHeight = scaledDim(m.progressBarHeight);
  m.statusBarVerticalMargin = scaledDim(m.statusBarVerticalMargin);
  m.keyboardKeyHeight = scaledTouchDim(m.keyboardKeyHeight);
  m.keyboardVerticalOffset = scaledDim(m.keyboardVerticalOffset);
  m.popupMarginX = scaledDim(m.popupMarginX);
  m.popupMarginY = scaledDim(m.popupMarginY);
  m.popupTextBaselineOffsetY = scaledDim(m.popupTextBaselineOffsetY);
  m.optionPopupItemSpacing = scaledDim(m.optionPopupItemSpacing);
  m.optionPopupInnerPadding = scaledDim(m.optionPopupInnerPadding);
  m.optionPopupSelectionHPadding = scaledDim(m.optionPopupSelectionHPadding);
  m.optionPopupSelectionVPadding = scaledDim(m.optionPopupSelectionVPadding);
  m.optionPopupTitleGap = scaledDim(m.optionPopupTitleGap);
  m.optionPopupDialogSideMargin = scaledDim(m.optionPopupDialogSideMargin);
  return m;
}

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics rawValues = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionHPadding = 8,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupTitleGap = 10,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = true,
                                 .optionPopupSelectionRadius = 0,
                                 .optionPopupSelectionLight = false,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
constexpr ThemeMetrics values = scaledMetrics(rawValues);
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  virtual int getListRowStep(bool hasSubtitle) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                                 int& index) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                               int selectedIndex) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true, const bool isPageBookmarked = false,
                     const bool pageCountEstimated = false) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
