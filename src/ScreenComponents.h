#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class GfxRenderer;

struct TabInfo {
  const char* label;
  bool selected;
};

class ScreenComponents {
 public:
  static const int BOOK_PROGRESS_BAR_HEIGHT = 4;

  static void drawBattery(const GfxRenderer& renderer, int left, int top, bool showPercentage = true);
  static void drawBookProgressBar(const GfxRenderer& renderer, size_t bookProgress);

  // Draw a horizontal tab bar with underline indicator for selected tab
  // Returns the height of the tab bar (for positioning content below)
  static int drawTabBar(const GfxRenderer& renderer, int y, const std::vector<TabInfo>& tabs);

  // Draw a scroll/page indicator on the right side of the screen
  // Shows up/down arrows and current page fraction (e.g., "1/3")
  static void drawScrollIndicator(const GfxRenderer& renderer, int currentPage, int totalPages, int contentTop,
                                  int contentHeight);

  /**
   * Draw a progress bar with percentage text.
   * @param renderer The graphics renderer
   * @param x Left position of the bar
   * @param y Top position of the bar
   * @param width Width of the bar
   * @param height Height of the bar
   * @param current Current progress value
   * @param total Total value for 100% progress
   */
  static void drawProgressBar(const GfxRenderer& renderer, int x, int y, int width, int height, size_t current,
                              size_t total);
};
