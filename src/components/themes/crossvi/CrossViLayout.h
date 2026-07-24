#pragma once

#include <algorithm>

#include "components/themes/BaseTheme.h"

// Pure geometry for the CrossVi Home bookplate. Keeping the measurements out
// of the renderer makes the X3 and X4 layouts cheap to verify in host tests.
struct CrossViLayout {
  Rect card;
  Rect cover;
  Rect details;
  Rect title;
  Rect stats;
  Rect progress;
  Rect progressBar;
  Rect progressLabel;
  Rect continueButton;
  Rect continueButtonWithoutProgress;

  static CrossViLayout calculate(const Rect tile) {
    constexpr int OUTER_PADDING = 20;
    constexpr int CARD_VERTICAL_PADDING = 8;
    constexpr int INNER_PADDING = 12;
    constexpr int INNER_GAP = 16;
    constexpr int COVER_LEFT_MARGIN = 8;
    constexpr int COVER_RIGHT_MARGIN = 4;
    constexpr int TITLE_HEIGHT = 58;
    constexpr int STATS_HEIGHT = 22;
    constexpr int PROGRESS_HEIGHT = 18;
    constexpr int BUTTON_HEIGHT = 38;
    constexpr int TITLE_STATS_GAP = 6;
    constexpr int STATS_PROGRESS_GAP = 12;
    constexpr int PROGRESS_BUTTON_GAP = 8;
    constexpr int NO_PROGRESS_BUTTON_GAP = 24;

    CrossViLayout out;
    out.card = Rect{tile.x + OUTER_PADDING, tile.y + CARD_VERTICAL_PADDING, std::max(0, tile.width - OUTER_PADDING * 2),
                    std::max(0, tile.height - CARD_VERTICAL_PADDING * 2)};

    const Rect inner{out.card.x + INNER_PADDING, out.card.y + INNER_PADDING,
                     std::max(0, out.card.width - INNER_PADDING * 2), std::max(0, out.card.height - INNER_PADDING * 2)};
    const int preferredCoverSlotWidth = tile.width >= 520 ? 112 : 104;
    const int maxCoverSlotWidth = std::max(0, inner.width - INNER_GAP - 160);
    const int coverSlotWidth = std::min(preferredCoverSlotWidth, maxCoverSlotWidth);
    const int coverLeftMargin = std::min(COVER_LEFT_MARGIN, coverSlotWidth);
    const int coverRightMargin = std::min(COVER_RIGHT_MARGIN, std::max(0, coverSlotWidth - coverLeftMargin));
    const int coverWidth = std::max(0, coverSlotWidth - coverLeftMargin - coverRightMargin);
    const int coverHeight = std::min(inner.height, coverWidth * 3 / 2);
    out.cover = Rect{inner.x + coverLeftMargin, inner.y + std::max(0, (inner.height - coverHeight) / 2), coverWidth,
                     coverHeight};

    const int detailsX = inner.x + coverSlotWidth + (coverSlotWidth > 0 ? INNER_GAP : 0);
    out.details = Rect{detailsX, inner.y, std::max(0, inner.x + inner.width - detailsX), inner.height};

    const int contentHeight = TITLE_HEIGHT + TITLE_STATS_GAP + STATS_HEIGHT + STATS_PROGRESS_GAP + PROGRESS_HEIGHT +
                              PROGRESS_BUTTON_GAP + BUTTON_HEIGHT;
    int y = out.details.y + std::max(0, (out.details.height - contentHeight) / 2);
    out.title = Rect{out.details.x, y, out.details.width, std::min(TITLE_HEIGHT, out.details.height)};
    y = std::min(out.details.y + out.details.height, out.title.y + out.title.height + TITLE_STATS_GAP);
    out.stats = Rect{out.details.x, y, out.details.width,
                     std::min(STATS_HEIGHT, std::max(0, out.details.y + out.details.height - y))};
    y = std::min(out.details.y + out.details.height, out.stats.y + out.stats.height + STATS_PROGRESS_GAP);
    out.progress = Rect{out.details.x, y, out.details.width,
                        std::min(PROGRESS_HEIGHT, std::max(0, out.details.y + out.details.height - y))};

    constexpr int PROGRESS_LABEL_WIDTH = 42;
    constexpr int PROGRESS_LABEL_GAP = 8;
    const int progressLabelWidth = std::min(PROGRESS_LABEL_WIDTH, out.progress.width);
    const int progressGap = out.progress.width > progressLabelWidth ? PROGRESS_LABEL_GAP : 0;
    out.progressBar = Rect{out.progress.x, out.progress.y,
                           std::max(0, out.progress.width - progressLabelWidth - progressGap), out.progress.height};
    out.progressLabel = Rect{out.progress.x + out.progress.width - progressLabelWidth, out.progress.y,
                             progressLabelWidth, out.progress.height};

    const int buttonWidth = std::min(176, out.details.width);
    y = std::min(out.details.y + out.details.height,
                 out.progress.y + out.progress.height + PROGRESS_BUTTON_GAP);
    out.continueButton = Rect{out.details.x, y, buttonWidth,
                              std::min(BUTTON_HEIGHT, std::max(0, out.details.y + out.details.height - y))};
    const int noProgressY = std::min(out.details.y + out.details.height,
                                    out.stats.y + out.stats.height + NO_PROGRESS_BUTTON_GAP);
    out.continueButtonWithoutProgress =
        Rect{out.details.x, noProgressY, buttonWidth,
             std::min(BUTTON_HEIGHT, std::max(0, out.details.y + out.details.height - noProgressY))};
    return out;
  }
};
