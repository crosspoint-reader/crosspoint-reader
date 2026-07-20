#pragma once

#include <algorithm>

#include "components/themes/BaseTheme.h"

// Pure geometry for the Dashboard home theme. Keeping this independent from
// GfxRenderer makes both device layouts testable on a host build.
struct DashboardLayout {
  Rect content;
  Rect cover;
  Rect stats;
  Rect title;
  Rect progress;
  Rect footer;

  static DashboardLayout calculate(const Rect tile) {
    constexpr int OUTER_PADDING = 20;
    constexpr int INNER_GAP = 16;
    constexpr int COVER_HEIGHT = 252;
    constexpr int COVER_WIDTH = 168;
    constexpr int TITLE_HEIGHT = 48;
    constexpr int PROGRESS_HEIGHT = 14;
    constexpr int ROW_GAP = 10;

    DashboardLayout out;
    out.content = Rect{tile.x + OUTER_PADDING, tile.y + 8, std::max(0, tile.width - OUTER_PADDING * 2),
                       std::max(0, tile.height - 16)};

    const int coverHeight = std::min(COVER_HEIGHT, out.content.height);
    const int coverWidth = std::min(COVER_WIDTH, out.content.width);
    out.cover = Rect{out.content.x, out.content.y, coverWidth, coverHeight};

    const int statsX = out.cover.x + out.cover.width + INNER_GAP;
    out.stats = Rect{statsX, out.content.y, std::max(0, out.content.x + out.content.width - statsX), coverHeight};

    const int contentBottom = out.content.y + out.content.height;
    const int titleY = std::min(contentBottom, out.cover.y + out.cover.height + ROW_GAP);
    out.title =
        Rect{out.content.x, titleY, out.content.width, std::min(TITLE_HEIGHT, std::max(0, contentBottom - titleY))};

    const int progressY = std::min(contentBottom, out.title.y + out.title.height + ROW_GAP);
    out.progress = Rect{out.content.x, progressY, out.content.width,
                        std::min(PROGRESS_HEIGHT, std::max(0, contentBottom - progressY))};

    const int footerY = std::min(contentBottom, out.progress.y + out.progress.height + ROW_GAP);
    out.footer = Rect{out.content.x, footerY, out.content.width, std::max(0, contentBottom - footerY)};
    return out;
  }
};
