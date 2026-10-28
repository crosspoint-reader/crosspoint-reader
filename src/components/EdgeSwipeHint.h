#pragma once

#include "util/EdgeSwipe.h"

class GfxRenderer;

// Caller holds RenderLock; the next activity paint replaces the hint on glass.
void drawEdgeSwipeHint(const GfxRenderer& renderer, edge_swipe::Edge edge, int startX, int startY);
