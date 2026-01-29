#pragma once

#include <functional>
#include <vector>

#include "CrossPointSettings.h"

class GfxRenderer;
struct RecentBookWithCover;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  // Constructor for explicit initialization
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
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeRecentBooksCount;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int versionTextRightX;
  int versionTextY;

  int bookProgressBarHeight;
};

struct PopupCallbacks {
  std::function<void()> setup;
  std::function<void(int)> update;
};

class UITheme {
 private:
  static const ThemeMetrics* currentMetrics;

 public:
  static void initialize();
  static void setTheme(CrossPointSettings::UI_THEME type);
  static const ThemeMetrics& getMetrics() { return *currentMetrics; }
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints);

  static void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  static void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true);
  static void drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                              const char* btn4);
  static void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn);
  static void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                       const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                       const std::function<std::string(int index)>& rowIcon, bool hasValue,
                       const std::function<std::string(int index)>& rowValue);
  static void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title);
  static void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs);
  static void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBookWithCover>& recentBooks,
                                  const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                  bool& bufferRestored, std::function<bool()> storeCoverBuffer);
  static void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                             const std::function<std::string(int index)>& buttonLabel, bool hasIcon,
                             const std::function<std::string(int index)>& rowIcon);
  static void drawPopup(GfxRenderer& renderer, const char* message);
  static PopupCallbacks drawPopupWithProgress(GfxRenderer& renderer, const std::string& title);
  static void drawBookProgressBar(const GfxRenderer& renderer, const size_t bookProgress);
};