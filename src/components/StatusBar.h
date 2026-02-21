#pragma once
#include <GfxRenderer.h>

namespace StatusBar {
const uint8_t statusBarMargin = 19;
const uint8_t progressBarMarginTop = 1;

uint8_t getStatusBarHeight();

void renderStatusBar(GfxRenderer& renderer, const int marginRight, const int marginBottom, const int marginLeft,
                     const float bookProgress, const int currentPage, const int pageCount, std::string title,
                     const int paddingBottom = 0);
};  // namespace StatusBar
