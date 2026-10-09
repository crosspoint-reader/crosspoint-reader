#pragma once

#include <memory>

#include "components/themes/BaseTheme.h"
#include "util/EdgeSwipe.h"

class GfxRenderer;
class EdgeSwipeIndicator {
 public:
  enum class Cleanup { None, RedrawPage };
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
  Cleanup render(const GfxRenderer& renderer, const Input& input, uint32_t now);
  static constexpr uint32_t NO_SERVICE = UINT32_MAX;
  uint32_t serviceDelay(uint32_t now) const;

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
  bool grayscaleUnderlay = false;
  uint8_t orientation = 0;
  Cleanup clear(const GfxRenderer& renderer, bool cleanup);
};
