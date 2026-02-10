#pragma once
#include <GfxRenderer.h>

namespace StatusBar {
void renderStatusBar(GfxRenderer& renderer, const int marginRight, const int marginBottom, const int marginLeft,
                     const float bookProgress, const int currentPage, const int pageCount, std::string title,
                     const int paddingBottom = 0);
};  // namespace StatusBar
