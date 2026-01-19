#pragma once

#include <cstddef>
#include <cstdint>

#include "components/themes/BaseTheme.h"

class GfxRenderer;

class RoundedTheme : public BaseTheme {
 public:
  // Property getters
  Rect getWindowContentFrame(GfxRenderer& renderer) override;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) override;
  void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                const std::function<std::string(int index)>& rowIcon, bool hasValue,
                const std::function<std::string(int index)>& rowValue) override;

  void drawWindowFrame(GfxRenderer& renderer, Rect rect, bool isPopup, const char* title) override;
  void drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title) override;
};
