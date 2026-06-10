#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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
  bool homeShowContinueReadingHeader;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;

  int keyboardKeyWidth;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  int keyboardBottomKeyHeight;
  int keyboardBottomKeySpacing;
  bool keyboardBottomAligned;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;
  int keyboardKeyCornerRadius;
  bool keyboardFillUnselected;
  bool keyboardOutlineAllUnselected;
  bool keyboardDrawSpecialOutlineWhenUnselected;
  int keyboardSecondaryLabelRightPadding;
  int keyboardSecondaryLabelTopPadding;
  int keyboardMinArrowHeadSize;

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

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;
};

enum class ThemeHomeRecentsType { Default, None, CoverStrip, Card };
enum class ThemeBookRef { Previous, Selected, Next, Index };
enum class ThemeSlotX { Padding, Center, RightPadding };
enum class ThemeSlotY { Top, Center };
enum class ThemeMenuSelectionStyle { Fill, Outline, Triangle, Underline, Pill };
enum class ThemeButtonHintsStyle { Buttons, Shapes, Groups };

struct ThemeTitleSpec {
  bool enabled = false;
  int fontId = 12;
  bool bold = true;
  int maxLines = 2;
  int offsetY = 12;
};

struct ThemeCoverSlotSpec {
  ThemeBookRef book = ThemeBookRef::Selected;
  int bookIndex = 0;
  ThemeSlotX x = ThemeSlotX::Center;
  ThemeSlotY y = ThemeSlotY::Top;
  int height = 300;
  int widthPercent = 62;
  int xOffset = 0;
  int yOffset = 0;
  bool selected = false;
  ThemeTitleSpec title;
};

struct ThemeHomeRecentsSpec {
  ThemeHomeRecentsType type = ThemeHomeRecentsType::Default;
  int maxBooks = 1;
  bool wrap = false;
  bool drawPanel = false;
  int panelCornerRadius = 6;
  int panelInsetX = 0;
  int selectionLineWidth = 3;
  int inactiveSelectionLineWidth = 0;
  int selectionCornerRadius = 6;
  std::vector<ThemeCoverSlotSpec> slots;
};

struct ThemeButtonMenuSpec {
  bool enabled = false;
  int fontId = 12;
  bool bold = false;
  bool centeredText = false;
  bool centerVertically = false;
  bool showIcons = true;
  int panelWidth = 0;
  bool drawPanel = false;
  int panelCornerRadius = 3;
  ThemeMenuSelectionStyle selectionStyle = ThemeMenuSelectionStyle::Fill;
  int selectionCornerRadius = 6;
  int selectionInset = 16;
  bool selectedTextInverted = false;
  bool selectionFillBlack = false;
  int rowPaddingX = 16;
  int textInsetX = 16;
};

struct ThemeListSpec {
  bool enabled = false;
  int fontId = 10;
  bool bold = false;
  int subtitleFontId = 0;
  int valueFontId = 0;
  bool showIcons = true;
  int iconSize = 0;
  int textGap = 8;
  ThemeMenuSelectionStyle selectionStyle = ThemeMenuSelectionStyle::Fill;
  int selectionCornerRadius = 6;
  bool selectionFill = true;
  bool selectionOutline = false;
  bool selectedTextInverted = false;
  bool rowBackgrounds = false;
  bool centerSingleLineRows = false;
  bool subtitleRowAutoHeight = false;
  bool centerValueVertically = false;
  int rowSidePadding = 0;
  int rowGap = 0;
  int textInsetX = 8;
  int selectionInsetX = 0;
  int selectionInsetY = 0;
  int titleOffsetY = 7;
  int subtitleOffsetY = 30;
  int subtitleTopPadding = 10;
  int subtitleBottomPadding = 10;
  int subtitleInterLineGap = 4;
  int valueOffsetY = 6;
  int subtitleValueOffsetY = 16;
  int iconOffsetY = 0;
};

struct ThemeButtonHintsSpec {
  bool enabled = false;
  int fontId = 0;
  bool bold = false;
  int buttonWidth = 80;
  int smallButtonHeight = 15;
  int cornerRadius = 6;
  bool fill = true;
  bool outline = true;
  bool drawEmpty = true;
  bool shapes = false;
  ThemeButtonHintsStyle style = ThemeButtonHintsStyle::Buttons;
  int sidePadding = 20;
  int groupGap = 10;
  int bottomMargin = 10;
  int innerPadding = 16;
  int shapeSize = 18;
  int textOffsetY = 7;
};

struct ThemeTabBarSpec {
  bool enabled = false;
  int fontId = 10;
  bool bold = false;
  bool equalWidth = false;
  ThemeMenuSelectionStyle selectionStyle = ThemeMenuSelectionStyle::Fill;
  int selectedCornerRadius = 6;
  bool selectedTextInverted = true;
  bool drawDivider = true;
  int horizontalInset = 2;
};

struct ThemeHeaderSpec {
  bool enabled = false;
  int fontId = 12;
  bool bold = true;
  bool centeredTitle = false;
  bool showDivider = true;
  int titleOffsetY = 0;
  int batteryOffsetY = 5;
};

enum UIIcon { None = 0, Folder, Text, Image, Book, File, Recent, Settings, Transfer, Library, Wifi, Hotspot, Bookmark };

using ThemeIconMap = std::map<UIIcon, std::string>;

enum class KeyboardKeyType { Normal, Shift, Mode, Space, Del, Ok, Disabled };

// The one firmware theme. All visual variation comes from ThemeMetrics and the
// component specs (populated by SD themes); there are no theme subclasses.

namespace BaseMetrics {
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

class BaseTheme final {
 public:
  explicit BaseTheme(const ThemeMetrics* metrics = &BaseMetrics::values,
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
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (UI headers)
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  int getListPageItems(int contentHeight, bool hasSubtitle) const;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = nullptr,
                const std::function<UIIcon(int index)>& rowIcon = nullptr,
                const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = nullptr) const;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr) const;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel = nullptr) const;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, bool selected) const;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer, bool coverStripSelected = true) const;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const;
  bool homeCoverCacheDependsOnSelector() const {
    return homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::CoverStrip;
  }
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const;
  void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                       const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                       bool inactiveSelection = false) const;
  bool showsFileIcons() const { return true; }

  // Shared constants and helpers for battery drawing
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);

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
  const ThemeMetrics& metrics() const { return metrics_ ? *metrics_ : BaseMetrics::values; }
  bool hasThemeIcon(UIIcon icon) const;
  bool drawThemeIcon(const GfxRenderer& renderer, UIIcon icon, int x, int y, int size) const;
  void drawCoverStripRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                             int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool bufferRestored,
                             std::function<bool()> storeCoverBuffer, bool coverStripSelected) const;
  // Single centered book card: aspect-correct cover at fixed height, bookmark
  // ribbon, title/author block, and a Continue Reading label (the original
  // "classic" home look, selected by homeRecents type "card").
  void drawCardRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                       int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool bufferRestored,
                       std::function<bool()> storeCoverBuffer) const;
};
