#include "EdgeSwipeHint.h"

#include <GfxRenderer.h>

#if FREEINK_CAP_TOUCH
#include "icons/edgeSwipeIcons.h"
#endif

void drawEdgeSwipeHint(const GfxRenderer& renderer, edge_swipe::Edge edge, int startX, int startY) {
#if FREEINK_CAP_TOUCH
  using namespace edge_swipe;
  const int sw = renderer.getScreenWidth(), sh = renderer.getScreenHeight();
  const uint8_t* bits = nullptr;
  const uint8_t* mask = nullptr;
  int x = 0, y = 0, w = 48, h = 144;
  switch (edge) {
    case Edge::Left:
      bits = leftBits;
      mask = leftMask;
      y = std::clamp(startY - 72, 0, std::max(0, sh - h));
      break;
    case Edge::Right:
      bits = rightBits;
      mask = rightMask;
      x = std::max(0, sw - w);
      y = std::clamp(startY - 72, 0, std::max(0, sh - h));
      break;
    case Edge::Bottom:
      bits = homeBits;
      mask = homeMask;
      w = 144;
      h = 48;
      x = std::clamp(startX - 72, 0, std::max(0, sw - w));
      y = std::max(0, sh - h);
      break;
    case Edge::Top:
    case Edge::None:
      return;
  }
  // Rotation can add one byte per physical row. RenderLock serializes this shared scratch.
  static uint8_t underlay[1024];
  if (!renderer.copyRegionToBuffer(x, y, w, h, underlay, sizeof(underlay))) return;
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      const int offset = row * (w / 8) + col / 8;
      const uint8_t bit = 0x80 >> (col % 8);
      if (mask[offset] & bit) renderer.drawPixel(x + col, y + row, !(bits[offset] & bit));
    }
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  renderer.copyBufferToRegion(x, y, w, h, underlay, sizeof(underlay));
#else
  (void)renderer;
  (void)edge;
  (void)startX;
  (void)startY;
#endif
}
