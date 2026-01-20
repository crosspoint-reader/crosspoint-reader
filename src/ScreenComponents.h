#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

class ScreenComponents {
 public:
  static constexpr int POPUP_DEFAULT_MIN_HEIGHT = 72;
  static constexpr int POPUP_DEFAULT_BAR_HEIGHT = 6;
  static constexpr int POPUP_DEFAULT_MIN_WIDTH = 200;

  struct PopupLayout {
    int x;
    int y;
    int width;
    int height;

    int barX;
    int barY;
    int barWidth;
    int barHeight;
  };

  static void drawBattery(const GfxRenderer& renderer, int left, int top, bool showPercentage = true);

  static PopupLayout drawPopup(const GfxRenderer& renderer, const char* message, int y = 117,
                               int minWidth = POPUP_DEFAULT_MIN_WIDTH,
                               int minHeight = POPUP_DEFAULT_MIN_HEIGHT);

  static void fillPopupProgress(const GfxRenderer& renderer, const PopupLayout& layout, int progress);

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
