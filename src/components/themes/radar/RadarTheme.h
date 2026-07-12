#pragma once

#include "components/themes/noir/NoirTheme.h"

namespace RadarMetrics {
// Reuse Noir metrics — the primary differentiation is on the home screen via RadarHomeRenderer.
constexpr ThemeMetrics values = {
    .batteryWidth = 16,
    .batteryHeight = 12,
    .topPadding = 5,
    .batteryBarHeight = 40,
    .headerHeight = 72,
    .verticalSpacing = 12,
    .contentSidePadding = 20,
    .listRowHeight = 40,
    .listWithSubtitleRowHeight = 60,
    .menuRowHeight = 56,
    .menuSpacing = 6,
    .tabSpacing = 8,
    .tabBarHeight = 40,
    .scrollBarWidth = 4,
    .scrollBarRightOffset = 5,
    .homeTopPadding = 50,
    .homeCoverHeight = 226,
    .homeCoverTileHeight = 242,
    .homeRecentBooksCount = 1,
    .buttonHintsHeight = 40,
    .sideButtonHintsWidth = 30,
    .progressBarHeight = 16,
    .progressBarMarginTop = 1,
    .statusBarHorizontalMargin = 5,
    .statusBarVerticalMargin = 19,
    .keyboardKeyWidth = 31,
    .keyboardKeyHeight = 50,
    .keyboardKeySpacing = 0,
    .keyboardBottomAligned = true,
    .keyboardCenteredText = true
};
}

// RadarTheme inherits all drawing from NoirTheme (inverted black/white look).
// The primary visual differentiation from Noir is the radar home screen
// rendered by RadarHomeRenderer in AppsMenuActivity.
class RadarTheme : public NoirTheme {};
