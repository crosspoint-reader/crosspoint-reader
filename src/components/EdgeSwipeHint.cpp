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
    case Edge::Right:
      bits = leftBits;
      mask = leftMask;
      x = edge == Edge::Right ? std::max(0, sw - w) : 0;
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
  BitmapRunReader bitReader(bits), maskReader(mask);
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; col += 8) {
      const uint8_t pixels = bitReader.next(), opaque = maskReader.next();
      for (int bit = 0; bit < 8; ++bit) {
        if (opaque & (0x80 >> bit)) {
          const int pixelX = edge == Edge::Right ? w - 1 - col - bit : col + bit;
          renderer.drawPixel(x + pixelX, y + row, !(pixels & (0x80 >> bit)));
        }
      }
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
