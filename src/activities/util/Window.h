#pragma once
#include <GfxRenderer.h>

void drawWindowFrame(GfxRenderer& renderer, int xMargin, int y, int height, bool hasShadow, const char* title);
void drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title);
void drawStatusBar(GfxRenderer& renderer);