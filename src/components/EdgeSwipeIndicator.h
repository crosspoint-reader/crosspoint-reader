#pragma once

#include <memory>

#include "components/themes/BaseTheme.h"
#include "util/EdgeSwipe.h"

class GfxRenderer;
class EdgeSwipeIndicator {
 public:
  struct Input {
    edge_swipe::State state;
    float dpi = 220;
    uint8_t orientation = 0;
    bool enabled = true;
  };
  // Called once on touch boards. The bounded snapshot cannot live on the small
  // render-task stack; retaining it avoids allocation during gestures.
  void begin();
  void pageChanged();
  void render(const GfxRenderer& renderer, const Input& input, uint32_t now);
  bool needsService() const { return visible || pending; }

 private:
  std::unique_ptr<uint8_t[]> underlay;
  Rect region;
  uint32_t contact = 0;
  uint32_t displayGeneration = 0;
  uint32_t refreshedAt = 0;
  uint32_t idleAt = 0;
  unsigned refreshes = 0;
  bool visible = false;
  bool pending = false;
  bool hasRefreshed = false;
  bool exhausted = false;
  bool committed = false;
  uint8_t orientation = 0;
  void clear(const GfxRenderer& renderer, bool cleanup);
};
